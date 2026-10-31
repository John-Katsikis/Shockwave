#pragma once

// RenderBackend: what a renderer plugin implements. Games and tools never see it; they
// use Renderer, which forwards to the current backend and can swap it for another (or a
// rebuilt copy of the same one) while running. See docs/Plugins.md.
//
// A plugin library defines a PluginDesc of kind "renderer" whose api points to a
// RendererPluginApi:
//
//     const RendererPluginApi api{.flags = kRendererDrawsGui, .create = ...};
//     const PluginDesc plugin{.kind = "renderer", .name = "vulkan", .description = "...", .api = &api};
//     SHOCKWAVE_PLUGIN(plugin)
//
// The contract, beyond what Renderer documents for each call:
// - Renderer picks resource ids, so they stay the same when backends swap. Mesh ids are
//   below maxMeshes(); texture ids are below maxTextures(), and 0 is the backend's own
//   white texture, which it makes when it starts.
// - Every setting (vsync, render scale, lighting...) is applied again after a swap, so a
//   new backend starts from its defaults and doesn't need to read the old one's state.
// - The destructor waits for the GPU and frees everything. It runs before the library
//   is unloaded.

#include <Renderer/GpuSelection.h>
#include <Renderer/Mesh.h>
#include <Renderer/Particles.h>
#include <Renderer/PostProcess.h>
#include <Renderer/Decals.h>
#include <Renderer/ReflectionProbes.h>
#include <Renderer/RenderTypes.h>
#include <Renderer/Clusters.h>
#include <Renderer/Temporal.h>
#include <Renderer/TextureData.h>

#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace shockwave {

class JobSystem;
class Window;

class RenderBackend {
public:
    virtual ~RenderBackend() = default;

    // ---- Resources: Renderer picks the ids ----
    virtual uint32_t maxMeshes() const = 0;
    virtual uint32_t maxTextures() const = 0;
    virtual void createMesh(MeshHandle id, std::span<const Vertex> vertices, std::span<const uint32_t> indices) = 0;
    virtual void destroyMesh(MeshHandle id) = 0;
    // Right after createMesh for a skinned mesh: one VertexSkin per vertex.
    virtual void setMeshSkin(MeshHandle id, std::span<const VertexSkin> skin) = 0;
    virtual void
    createTexture(TextureHandle id, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) = 0;
    virtual void
    updateTexture(TextureHandle id, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) = 0;
    virtual void destroyTexture(TextureHandle id) = 0;
    virtual uint64_t guiTexture(TextureHandle id) = 0;

    // ---- Optimised assets (docs/Optimisation.md); the defaults suit a simple backend ----
    // Right after createMesh: its simpler levels of detail, ranges of lodIndices. Ignoring
    // them always draws full detail.
    virtual void setMeshLods(MeshHandle, std::span<const MeshLod>, std::span<const uint32_t> /*lodIndices*/) {}
    virtual bool supportsTextureFormat(TextureFormat format) const { return format == TextureFormat::RGBA8; }
    // Only formats supportsTextureFormat accepts. The defaults use RGBA8's largest level.
    virtual void createTextureData(TextureHandle id, const TextureData& data) {
        if (data.format != TextureFormat::RGBA8 || data.levels.empty())
            throw std::runtime_error("this renderer can't load compressed textures");
        const auto& level = data.levels[0];
        createTexture(id, level.width, level.height, {data.bytes.data() + level.offset, level.size}, data.srgb);
    }
    virtual void updateTextureData(TextureHandle id, const TextureData& data) {
        if (data.format != TextureFormat::RGBA8 || data.levels.empty())
            throw std::runtime_error("this renderer can't load compressed textures");
        const auto& level = data.levels[0];
        updateTexture(id, level.width, level.height, {data.bytes.data() + level.offset, level.size}, data.srgb);
    }
    virtual void setLodSettings(const LodSettings&) {}
    // Cube textures (G07) come through createTextureData with TextureShape::Cube; a backend
    // without a cube table refuses them here and Renderer says so.
    virtual bool supportsTextureShape(TextureShape shape) const { return shape == TextureShape::Texture2D; }

    // ---- Material bindings (G02); the defaults suit a backend that ignores them ----
    // Right after createMesh: the mesh's second UV set, one per vertex. Ignoring it reads
    // UV0 wherever a material asks for UV1.
    virtual void setMeshUv1(MeshHandle, std::span<const glm::vec2> /*uv1*/) {}
    // Lightmap UVs (G05). A backend without them draws baked receivers with the environment.
    virtual void setMeshLightmapUv(MeshHandle, std::span<const glm::vec2> /*uv*/) {}
    // Once per distinct SamplerDesc; id 0 (the default) is never sent. Any thread.
    virtual void createSampler(SamplerHandle, const SamplerDesc&) {}
    virtual void setAnisotropy(uint32_t /*1..16*/) {}
    // A02: Low leaves DrawMaterial::layers' detail out of later draws.
    virtual void setMaterialQuality(MaterialQuality) {}
    virtual LodStats lodStats() const { return {}; }

    // ---- Per frame ----
    virtual void setClearColor(const Color& color) = 0;
    virtual void setCamera(const glm::mat4& view, const glm::mat4& projection) = 0;
    // Before setCamera: which view the camera belongs to (RenderTypes.h). A different view
    // than last frame's means no history carries over.
    virtual void setView(RenderViewId) {}
    // Renderer::cutCamera: drop temporal history this frame.
    virtual void cutCamera() {}
    // key: which object this is (RenderTypes.h); invalid for draws without one.
    virtual void drawMesh(
        MeshHandle mesh, const glm::mat4& transform, TextureHandle texture, const glm::vec4& tint,
        const DrawMaterial& material, const RenderInstanceKey& key) = 0;
    virtual void addLight(const LightDesc& light) = 0;
    // This frame's particles (Renderer/Particles.h); copy what you need before returning.
    virtual void drawParticles(const ParticleDrawList& particles) = 0;
    // Renderer::drawSkinnedMesh; joints must be copied before returning.
    virtual void drawSkinnedMesh(
        MeshHandle mesh, const glm::mat4& transform, std::span<const glm::mat4> joints, TextureHandle texture,
        const glm::vec4& tint, const DrawMaterial& material, const RenderInstanceKey& key) = 0;

    // ---- Settings ----
    virtual void setGrid(const GridSettings& grid) = 0;
    virtual void setLighting(const LightingDesc& lighting) = 0;
    virtual void setDebugView(DebugView view) = 0;
    virtual void setShadowQuality(ShadowQuality quality) = 0;
    // A backend without these reports them off in effectiveSettings.
    virtual void setAntiAliasing(AntiAliasing) {}
    virtual void setAmbientOcclusion(AmbientOcclusion) {}
    virtual void setBloom(BloomQuality, float /*scale, already clamped to 0..1*/) {}
    virtual void setRenderScale(float scale) = 0;   // already clamped by Renderer
    virtual void setViewport(uint32_t width, uint32_t height) = 0;
    virtual void setVsync(bool vsync) = 0;
    virtual void setLowLatency(bool lowLatency) = 0;

    // ---- Results of the last frame ----
    virtual uint32_t lightCount() const = 0;
    virtual uint32_t droppedLights() const = 0;
    virtual ShadowStats shadowStats() const = 0;
    virtual std::string frameGraph() const = 0;
    virtual uint32_t renderWidth() const = 0;
    virtual uint32_t renderHeight() const = 0;
    virtual uint64_t sceneTexture() const = 0;
    virtual const std::vector<PassTiming>& passTimings() const = 0;
    virtual float gpuFrameMs() const = 0;
    virtual float aspectRatio() const = 0;
    virtual const char* gpuName() const = 0;
    virtual uint32_t drawCount() const = 0;
    // Of those, how many were outside the camera's view and not drawn (shadows may still be).
    virtual uint32_t culledDraws() const { return 0; }
    virtual ParticleStats particleStats() const { return {}; }
    // A backend without soft particles, distortion or scene lighting for particles ignores
    // the settings it can't follow (and skips distortion batches).
    virtual void setParticleSettings(const ParticleSettings&) {}
    // A05, A06: a backend without grading, metering or fog draws the scene without them.
    virtual void setPostProcess(const PostProcessDesc&) {}
    virtual void setPostProcessSettings(const PostProcessSettings&) {}
    virtual PostProcessStats postProcessStats() const { return {}; }
    // Of drawCount, how many came without a RenderInstanceKey (so have no history).
    virtual uint32_t unkeyedDraws() const { return 0; }
    virtual TemporalStats temporalStats() const { return {}; }

    // ---- Capabilities, effective settings and memory (RenderTypes.h) ----
    // What the device supports, for picking effects and fallbacks. Fill in backend.
    virtual RendererCapabilities capabilities() const { return {}; }
    // Every setting Renderer passed in (render_scale, shadow_quality, vsync, low_latency,
    // and later effects), as requested and as actually running.
    virtual std::vector<EffectiveSetting> effectiveSettings() const { return {}; }
    // GPU memory by category; Renderer adds its own CPU copies.
    virtual RendererMemoryReport memoryReport() const { return {}; }

    // ---- Local reflection probes (G07, Renderer/ReflectionProbes.h) ----
    // Kept until changed; each later draw picks up to two by its bounds. A backend without
    // them draws every reflection from the global environment.
    virtual void setReflectionProbes(std::span<const ReflectionProbeDesc>) {}
    virtual void setLocalReflections(bool) {}
    // The next frame's HDR scene colour, read back once it's finished; that frame draws
    // without local reflections and with the lit view. False (and why) without a GPU.
    virtual bool requestSceneCapture(std::string& error) {
        error = "this renderer can't capture the scene (reflection capture needs a GPU renderer)";
        return false;
    }
    virtual bool sceneCapturePending() const { return false; }
    // The last capture, once it isn't pending; moved out by the call.
    virtual SceneCapture takeSceneCapture() { return {0, 0, {}, "nothing was captured"}; }

    // ---- Projected decals (G08, Renderer/Decals.h) ----
    // This frame's decals; same thread as setCamera, cleared after each frame. A backend
    // without them draws none (and reports decals_quality off).
    virtual void drawDecals(std::span<const DecalDesc>) {}
    virtual void setDecalQuality(DecalQuality) {}
    virtual DecalStats decalStats() const { return {}; }

    // ---- Clustered local lighting (G11, Renderer/Clusters.h) ----
    // A backend without clusters loops over every light (and reports brute_force).
    virtual void setLightCulling(LightCulling) {}
    virtual ClusterStats clusterStats() const { return {}; }

    // ---- Screenshots ----
    virtual void requestScreenshot(const std::string& path) = 0;
    virtual bool screenshotPending() const = 0;
    virtual const std::string& screenshotResult() const = 0;

    // ---- Presentation ----
    virtual float waitForPresent() = 0;
    virtual const char* presentTiming() const = 0;
    virtual const char* presentModeName() const = 0;

    // ---- Engine calls ----
    virtual bool beginFrame() = 0;
    virtual void endFrame() = 0;
    virtual void onResize() = 0;
    virtual void waitIdle() = 0;
    virtual void initGui() = 0;
    virtual void shutdownGui() = 0;
    virtual void newGuiFrame() = 0;
    virtual void renderGuiWindows() = 0;

    // ---- Game UI overlay (RenderTypes.h) ----
    // The size in pixels the overlay draws at: the editor viewport, or the window.
    virtual glm::uvec2 uiOutputSize() const = 0;
    // This frame's overlay. It's drawn after tonemapping into the display image, so the
    // window, the editor viewport and screenshots all show it. The backend copies what it
    // needs before returning. Main thread, between beginFrame and endFrame.
    virtual void drawUi(const UiDrawList& list) = 0;

    // ---- GPU (GpuSelection.h) ----
    // Every GPU the backend saw and why it picked the one it uses; null when it has no GPU.
    virtual const GpuChoice* gpuChoice() const { return nullptr; }
};

struct RenderBackendDesc {
    Window& window;
    JobSystem& jobs;
    bool vsync = true;
};

struct RendererPluginApi {
    uint32_t flags = 0;
    // Throws std::exception with a readable message when it can't start (no GPU, say).
    std::unique_ptr<RenderBackend> (*create)(const RenderBackendDesc& desc) = nullptr;
};

}   // namespace shockwave
