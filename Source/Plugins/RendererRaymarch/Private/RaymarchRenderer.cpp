// The software ray marcher: a renderer plugin that draws with no GPU features at all.
// player --renderer raymarch (see Renderer/RenderBackend.h and docs/Plugins.md).
//
// How a frame is made:
//   1. createMesh bakes each mesh into a signed distance field (DistanceField.h) and a
//      sparse voxel octree over it (SparseVoxelOctree.h), once.
//   2. drawMesh and addLight collect this frame's instances and lights.
//   3. endFrame shoots one ray per pixel at a fraction of the window's resolution, spread
//      across the job system. Each ray is tested against every instance's box; inside a
//      box it skips the octree's empty nodes and sphere traces the field near the surface.
//   4. Hits are shaded with the scene's lights (soft shadows traced through the same
//      fields), tonemapped, and shown through SDL.
//
// It's a skeleton to build on, so it keeps things simple: no textures or UVs (surfaces
// take the draw's tint and the mesh's vertex colour), skinned meshes in their bind pose,
// no particles, game UI or editor GUI, and a brute-force loop over instances where a
// scene-level BVH or octree would go. Each of those is marked "Next:" below.

#include "DistanceField.h"
#include "SparseVoxelOctree.h"

#include <Core/Jobs.h>
#include <Core/Log.h>
#include <Core/Plugin.h>
#include <Platform/Window.h>
#include <Renderer/RenderBackend.h>

#include <SDL3/SDL.h>
#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace shockwave {
namespace {

using raymarch::DistanceField;
using raymarch::SparseVoxelOctree;

constexpr uint32_t kGridSize = 32;   // distance field cells per side, per mesh
constexpr float kPixelScale = 0.25f;   // rays per window pixel, per axis (times render scale)
constexpr int kMaxSteps = 128;   // per ray, per instance
constexpr int kShadowSteps = 48;

struct MeshVolume {
    DistanceField field;
    SparseVoxelOctree octree;
};

// One drawMesh, ready for tracing.
struct Instance {
    std::shared_ptr<const MeshVolume> volume;
    glm::mat4 toLocal{1.0f};   // world -> mesh space
    glm::mat3 normalToWorld{1.0f};
    float minScale = 1.0f;   // the transform's smallest scale: mesh distances times this are safe in world units
    glm::vec3 albedo{1.0f};
    glm::vec3 emissive{0.0f};
    float alpha = 1.0f;
};

struct Hit {
    float t = 0.0f;
    const Instance* instance = nullptr;
    glm::vec3 localPoint{0.0f};
};

glm::vec3 tonemapAces(glm::vec3 x) {
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return glm::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
}

float toSrgb(float linear) {
    return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

// Where a ray (local origin o, local direction step d per world unit) enters and leaves a
// box, in world distance along the ray.
bool slab(const glm::vec3& o, const glm::vec3& d, const glm::vec3& lo, const glm::vec3& hi, float& t0, float& t1) {
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(d[axis]) < 1e-12f) {
            if (o[axis] < lo[axis] || o[axis] > hi[axis]) return false;
            continue;
        }
        float a = (lo[axis] - o[axis]) / d[axis], b = (hi[axis] - o[axis]) / d[axis];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t1 < t0) return false;
    }
    return true;
}

class RaymarchRenderer final : public RenderBackend {
public:
    RaymarchRenderer(Window& window, JobSystem& jobs) : m_window(window), m_jobs(jobs) {
        m_timings.push_back({"raymarch (CPU)", 0.0f});
    }
    ~RaymarchRenderer() override {
        if (m_texture) SDL_DestroyTexture(m_texture);
        if (m_sdl) SDL_DestroyRenderer(m_sdl);
    }

    // ---- Resources ----
    uint32_t maxMeshes() const override { return 1u << 20; }
    uint32_t maxTextures() const override { return 1u << 20; }
    void createMesh(MeshHandle id, std::span<const Vertex> vertices, std::span<const uint32_t> indices) override {
        const auto start = std::chrono::steady_clock::now();
        auto volume = std::make_shared<MeshVolume>();
        volume->field = DistanceField::bake(vertices, indices, kGridSize, &m_jobs);
        volume->octree.build(volume->field);
        SW_LOG(
            LogRender, Verbose, "raymarch: mesh {} baked: {}^3 field, {} octree nodes, {:.1f} ms", id.id, kGridSize,
            volume->octree.nodeCount(),
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        std::lock_guard lock(m_meshMutex);
        m_meshes[id.id] = std::move(volume);
    }
    void destroyMesh(MeshHandle id) override {
        std::lock_guard lock(m_meshMutex);
        m_meshes.erase(id.id);
    }
    void setMeshSkin(MeshHandle, std::span<const VertexSkin>) override {}   // Next: rebake or deform skinned fields
    // Next: keep textures and the meshes' UVs, and sample base colour at the hit.
    void createTexture(TextureHandle, uint32_t, uint32_t, std::span<const uint8_t>, bool) override {}
    void updateTexture(TextureHandle, uint32_t, uint32_t, std::span<const uint8_t>, bool) override {}
    void destroyTexture(TextureHandle) override {}
    uint64_t guiTexture(TextureHandle) override { return 0; }

    // ---- Per frame ----
    void setClearColor(const Color& color) override { m_clear = glm::vec3(color.r, color.g, color.b); }
    void setCamera(const glm::mat4& view, const glm::mat4& projection) override {
        m_view = view;
        m_projection = projection;
    }
    void drawMesh(
        MeshHandle mesh, const glm::mat4& transform, TextureHandle, const glm::vec4& tint, const DrawMaterial& material,
        const RenderInstanceKey&) override {
        std::shared_ptr<const MeshVolume> volume;
        {
            std::lock_guard lock(m_meshMutex);
            const auto found = m_meshes.find(mesh.id);
            if (found == m_meshes.end() || !found->second->field.valid()) return;
            volume = found->second;
        }
        const glm::mat3 linear(transform);
        const float det = glm::determinant(linear);
        if (!std::isfinite(det) || std::abs(det) < 1e-12f) return;
        Instance instance;
        instance.volume = std::move(volume);
        instance.toLocal = glm::inverse(transform);
        instance.normalToWorld = glm::inverseTranspose(linear);
        instance.minScale = std::min({glm::length(linear[0]), glm::length(linear[1]), glm::length(linear[2])});
        instance.albedo = glm::vec3(tint) * instance.volume->field.colour;
        instance.emissive = material.emissive;
        instance.alpha = tint.a;
        std::lock_guard lock(m_frameMutex);
        m_pending.push_back(std::move(instance));
    }
    void drawSkinnedMesh(
        MeshHandle mesh, const glm::mat4& transform, std::span<const glm::mat4>, TextureHandle texture,
        const glm::vec4& tint, const DrawMaterial& material, const RenderInstanceKey& key) override {
        drawMesh(mesh, transform, texture, tint, material, key);   // bind pose for now
    }
    void addLight(const LightDesc& light) override {
        std::lock_guard lock(m_frameMutex);
        m_pendingLights.push_back(light);
    }
    void drawParticles(const ParticleDrawList&) override {}   // Next: splat particles over the image

    // ---- Settings ----
    void setGrid(const GridSettings&) override {}
    void setLighting(const LightingDesc& lighting) override { m_lighting = lighting; }
    void setDebugView(DebugView view) override { m_debugView = view; }
    void setShadowQuality(ShadowQuality quality) override { m_shadows = quality != ShadowQuality::Off; }
    void setRenderScale(float scale) override { m_scale = scale; }
    void setViewport(uint32_t width, uint32_t height) override {
        m_viewportWidth = width;
        m_viewportHeight = height;
    }
    void setVsync(bool vsync) override {
        m_vsync = vsync;
        if (m_sdl) SDL_SetRenderVSync(m_sdl, vsync ? 1 : 0);
    }
    void setLowLatency(bool) override {}

    // ---- Results ----
    uint32_t lightCount() const override { return m_lastLights; }
    uint32_t droppedLights() const override { return 0; }
    ShadowStats shadowStats() const override { return {}; }
    std::string frameGraph() const override {
        return "raymarch (CPU): one ray per pixel at " + std::to_string(renderWidth()) + " x " +
               std::to_string(renderHeight()) + ", sphere traced through each mesh's distance field and octree, " +
               "then shown through SDL (" + (m_sdl ? SDL_GetRendererName(m_sdl) : "not shown") + ").\n";
    }
    uint32_t renderWidth() const override {
        return std::max(1u, static_cast<uint32_t>(float(outputWidth()) * m_scale * kPixelScale));
    }
    uint32_t renderHeight() const override {
        return std::max(1u, static_cast<uint32_t>(float(outputHeight()) * m_scale * kPixelScale));
    }
    uint64_t sceneTexture() const override { return 0; }
    const std::vector<PassTiming>& passTimings() const override { return m_timings; }
    float gpuFrameMs() const override { return 0.0f; }
    float aspectRatio() const override { return outputHeight() ? float(outputWidth()) / float(outputHeight()) : 1.0f; }
    const char* gpuName() const override { return "CPU (software ray marcher)"; }
    uint32_t drawCount() const override { return m_lastDraws; }

    // ---- Screenshots: the traced image, at its own resolution ----
    void requestScreenshot(const std::string& path) override {
        m_screenshotPath = path;
        m_screenshotPending = true;
        m_screenshotResult.clear();
    }
    bool screenshotPending() const override { return m_screenshotPending; }
    const std::string& screenshotResult() const override { return m_screenshotResult; }

    // ---- Presentation ----
    float waitForPresent() override { return 0.0f; }
    const char* presentTiming() const override { return "none"; }
    const char* presentModeName() const override { return m_sdl ? SDL_GetRendererName(m_sdl) : "none"; }

    bool beginFrame() override { return outputWidth() > 0 && outputHeight() > 0; }
    void endFrame() override {
        std::vector<Instance> instances;
        std::vector<LightDesc> lights;
        {
            std::lock_guard lock(m_frameMutex);
            instances.swap(m_pending);
            lights.swap(m_pendingLights);
        }
        m_lastDraws = static_cast<uint32_t>(instances.size());
        m_lastLights = static_cast<uint32_t>(lights.size());
        if (lights.empty()) {   // like the Vulkan renderer: a default sun keeps unlit scenes readable
            LightDesc sun;
            sun.type = LightDesc::Type::Directional;
            sun.direction = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f));
            sun.color = glm::vec3(1.0f);
            lights.push_back(sun);
        }
        const auto start = std::chrono::steady_clock::now();
        render(instances, lights);
        m_timings[0].gpuMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
        present();
        if (m_screenshotPending) saveScreenshot();
    }
    void onResize() override {}
    void waitIdle() override {}
    // Never called: without kRendererDrawsGui the engine doesn't hand it a GUI.
    void initGui() override {}
    void shutdownGui() override {}
    void newGuiFrame() override {}
    void renderGuiWindows() override {}

    glm::uvec2 uiOutputSize() const override { return {outputWidth(), outputHeight()}; }
    void drawUi(const UiDrawList&) override {}   // Next: draw the game UI over the traced image

private:
    uint32_t outputWidth() const {
        return m_viewportWidth ? m_viewportWidth : uint32_t(std::max(0, m_window.pixelWidth()));
    }
    uint32_t outputHeight() const {
        return m_viewportHeight ? m_viewportHeight : uint32_t(std::max(0, m_window.pixelHeight()));
    }

    // ---- Tracing ----

    // The nearest surface of one instance along a world ray, closer than maxT.
    bool traceInstance(
        const Instance& instance, const glm::vec3& origin, const glm::vec3& direction, float maxT, Hit& hit) const {
        const DistanceField& field = instance.volume->field;
        const SparseVoxelOctree& octree = instance.volume->octree;
        const glm::vec3 o = glm::vec3(instance.toLocal * glm::vec4(origin, 1.0f));
        const glm::vec3 d = glm::mat3(instance.toLocal) * direction;   // mesh units per world unit
        const float stretch = glm::length(d);
        float t0 = 0.0f, t1 = maxT;
        if (!slab(o, d, field.min, field.max, t0, t1)) return false;
        const float epsilon = field.cell * 0.15f;
        float t = t0;
        for (int step = 0; step < kMaxSteps && t <= t1; ++step) {
            const glm::vec3 p = o + d * t;
            const SparseVoxelOctree::Box box = octree.find(p);
            if (!box.surface) {
                // Empty node: jump to where the ray leaves it.
                float b0 = t, b1 = t1;
                if (!slab(o, d, box.min, box.max, b0, b1)) break;
                t = b1 + epsilon / stretch;
                continue;
            }
            const float distance = field.sample(p);
            if (distance < epsilon) {
                hit.t = t;
                hit.instance = &instance;
                hit.localPoint = p;
                return true;
            }
            t += std::max(distance, epsilon) / stretch;
        }
        return false;
    }

    // Next: a scene-level BVH or octree over the instances, instead of testing them all.
    bool trace(
        const std::vector<Instance>& instances, const glm::vec3& origin, const glm::vec3& direction, float maxT,
        Hit& best) const {
        bool any = false;
        for (const Instance& instance : instances) {
            Hit hit;
            if (traceInstance(instance, origin, direction, any ? best.t : maxT, hit)) {
                best = hit;
                any = true;
            }
        }
        return any;
    }

    // The scene's distance at a world point: a lower bound, for soft shadows.
    float sceneDistance(const std::vector<Instance>& instances, const glm::vec3& p) const {
        float nearest = 1e9f;
        for (const Instance& instance : instances) {
            const glm::vec3 local = glm::vec3(instance.toLocal * glm::vec4(p, 1.0f));
            nearest = std::min(nearest, instance.volume->field.sample(local) * instance.minScale);
        }
        return nearest;
    }

    // How much light gets from p towards a light: 1 lit .. 0 shadowed, with soft edges from
    // how closely the ray grazes other surfaces (the classic distance-field soft shadow).
    float softShadow(const std::vector<Instance>& instances, glm::vec3 p, const glm::vec3& toLight, float maxT) const {
        float lit = 1.0f, t = 0.05f;
        for (int step = 0; step < kShadowSteps && t < maxT; ++step) {
            const float d = sceneDistance(instances, p + toLight * t);
            if (d < 0.002f) return 0.0f;
            lit = std::min(lit, 8.0f * d / t);
            t += std::clamp(d, 0.02f, 1.0f);
        }
        return std::clamp(lit, 0.0f, 1.0f);
    }

    glm::vec3 shade(
        const std::vector<Instance>& instances, const std::vector<LightDesc>& lights, const Hit& hit,
        const glm::vec3& origin, const glm::vec3& direction) const {
        const Instance& instance = *hit.instance;
        const glm::vec3 point = origin + direction * hit.t;
        glm::vec3 normal = glm::normalize(instance.normalToWorld * instance.volume->field.gradient(hit.localPoint));
        if (glm::dot(normal, direction) > 0.0f) normal = -normal;   // open meshes seen from behind
        if (m_debugView == DebugView::Normals) return normal * 0.5f + 0.5f;
        glm::vec3 albedo = m_debugView == DebugView::Lighting ? glm::vec3(0.8f) : instance.albedo;
        if (m_debugView == DebugView::Unlit) return albedo + instance.emissive;

        // Hemisphere ambient, then each light with a soft shadow.
        glm::vec3 light = glm::mix(m_lighting.groundAmbient, m_lighting.skyAmbient, normal.y * 0.5f + 0.5f);
        const glm::vec3 lifted = point + normal * 0.01f;
        for (const LightDesc& l : lights) {
            glm::vec3 toLight;
            float attenuation = 1.0f, distance = 50.0f;
            if (l.type == LightDesc::Type::Directional) {
                toLight = -glm::normalize(l.direction);
            } else {
                const glm::vec3 offset = l.position - point;
                distance = glm::length(offset);
                if (distance >= l.range || distance < 1e-4f) continue;
                toLight = offset / distance;
                const float ratio = distance / l.range;
                const float window = std::clamp(1.0f - ratio * ratio * ratio * ratio, 0.0f, 1.0f);
                attenuation = window * window / (distance * distance + 1.0f);
                if (l.type == LightDesc::Type::Spot) {
                    const float cosAngle = glm::dot(-toLight, glm::normalize(l.direction));
                    const float span = std::max(l.cosInner - l.cosOuter, 1e-4f);
                    attenuation *= std::clamp((cosAngle - l.cosOuter) / span, 0.0f, 1.0f);
                }
            }
            const float facing = glm::dot(normal, toLight);
            if (facing <= 0.0f || attenuation <= 0.0f) continue;
            float shadow = 1.0f;
            if (m_shadows && l.castShadows)
                shadow = glm::mix(1.0f, softShadow(instances, lifted, toLight, distance), l.shadowStrength);
            light += l.color * (facing * attenuation * shadow);
        }
        return albedo * light + instance.emissive;
    }

    void render(const std::vector<Instance>& instances, const std::vector<LightDesc>& lights) {
        const uint32_t width = renderWidth(), height = renderHeight();
        m_pixels.resize(size_t(width) * height * 4);
        const glm::mat4 inverseViewProjection = glm::inverse(m_projection * m_view);
        const glm::vec3 origin = glm::vec3(glm::inverse(m_view)[3]);
        // Vulkan-style projections flip y; read the projection to know which way is up.
        const bool flipped = m_projection[1][1] < 0.0f;
        const float exposure = std::exp2(m_lighting.exposure);
        m_jobs.parallelFor(height, 1, [&](size_t begin, size_t end) {
            for (size_t y = begin; y < end; ++y) {
                const float ndcY = 1.0f - 2.0f * (float(y) + 0.5f) / float(height);
                for (uint32_t x = 0; x < width; ++x) {
                    const float ndcX = 2.0f * (float(x) + 0.5f) / float(width) - 1.0f;
                    // A point on the near plane (reverse-Z: depth 1) gives the ray's direction.
                    const glm::vec4 near = inverseViewProjection * glm::vec4(ndcX, flipped ? -ndcY : ndcY, 1.0f, 1.0f);
                    const glm::vec3 direction = glm::normalize(glm::vec3(near) / near.w - origin);
                    glm::vec3 colour = m_clear;
                    Hit hit;
                    if (trace(instances, origin, direction, 1000.0f, hit))
                        colour = shade(instances, lights, hit, origin, direction);
                    if (m_debugView != DebugView::Normals) colour = tonemapAces(colour * exposure);
                    uint8_t* out = &m_pixels[(y * width + x) * 4];
                    for (int c = 0; c < 3; ++c)
                        out[c] = static_cast<uint8_t>(std::clamp(toSrgb(colour[c]), 0.0f, 1.0f) * 255.0f + 0.5f);
                    out[3] = 255;
                }
            }
        });
        m_imageWidth = width;
        m_imageHeight = height;
    }

    // ---- Showing it ----

    void present() {
        if (!m_sdl && !m_presentFailed) {
            m_sdl = SDL_CreateRenderer(m_window.handle(), nullptr);
            if (!m_sdl) {
                m_presentFailed = true;   // screenshots still work
                SW_LOG(LogRender, Warning, "raymarch: can't show frames in this window: {}", SDL_GetError());
                return;
            }
            SDL_SetRenderVSync(m_sdl, m_vsync ? 1 : 0);
        }
        if (!m_sdl) return;
        if (!m_texture || m_textureWidth != m_imageWidth || m_textureHeight != m_imageHeight) {
            if (m_texture) SDL_DestroyTexture(m_texture);
            m_texture = SDL_CreateTexture(
                m_sdl, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, int(m_imageWidth), int(m_imageHeight));
            if (!m_texture) return;
            SDL_SetTextureScaleMode(m_texture, SDL_SCALEMODE_NEAREST);   // chunky pixels, on purpose
            m_textureWidth = m_imageWidth;
            m_textureHeight = m_imageHeight;
        }
        SDL_UpdateTexture(m_texture, nullptr, m_pixels.data(), int(m_imageWidth * 4));
        SDL_RenderClear(m_sdl);
        SDL_RenderTexture(m_sdl, m_texture, nullptr, nullptr);
        SDL_RenderPresent(m_sdl);
    }

    void saveScreenshot() {
        m_screenshotPending = false;
        SDL_Surface* surface = SDL_CreateSurfaceFrom(
            int(m_imageWidth), int(m_imageHeight), SDL_PIXELFORMAT_RGBA32, m_pixels.data(), int(m_imageWidth * 4));
        if (!surface || !SDL_SavePNG(surface, m_screenshotPath.c_str()))
            m_screenshotResult = std::string("couldn't save the screenshot: ") + SDL_GetError();
        if (surface) SDL_DestroySurface(surface);
    }

    Window& m_window;
    JobSystem& m_jobs;
    std::mutex m_meshMutex, m_frameMutex;
    std::unordered_map<uint32_t, std::shared_ptr<const MeshVolume>> m_meshes;
    std::vector<Instance> m_pending;
    std::vector<LightDesc> m_pendingLights;
    glm::mat4 m_view{1.0f}, m_projection{1.0f};
    glm::vec3 m_clear{0.02f, 0.02f, 0.03f};
    LightingDesc m_lighting;
    DebugView m_debugView = DebugView::Lit;
    bool m_shadows = true, m_vsync = true;
    float m_scale = 1.0f;
    uint32_t m_viewportWidth = 0, m_viewportHeight = 0;
    uint32_t m_lastDraws = 0, m_lastLights = 0;
    std::vector<PassTiming> m_timings;
    std::vector<uint8_t> m_pixels;
    uint32_t m_imageWidth = 0, m_imageHeight = 0;
    SDL_Renderer* m_sdl = nullptr;
    SDL_Texture* m_texture = nullptr;
    uint32_t m_textureWidth = 0, m_textureHeight = 0;
    bool m_presentFailed = false;
    std::string m_screenshotPath, m_screenshotResult;
    bool m_screenshotPending = false;
};

std::unique_ptr<RenderBackend> create(const RenderBackendDesc& desc) {
    return std::make_unique<RaymarchRenderer>(desc.window, desc.jobs);
}

const RendererPluginApi api{.flags = kRendererPresents, .create = create};
const PluginDesc plugin{
    .kind = "renderer",
    .name = "raymarch",
    .description = "Software ray marcher: signed distance fields and sparse voxel octrees on the CPU",
    .api = &api,
};

}   // namespace
}   // namespace shockwave

SHOCKWAVE_PLUGIN(shockwave::plugin)
