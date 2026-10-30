#include <Lighting/LightingBaker.h>

#include "BakeTracer.h"

#include <Core/Log.h>
#include <Engine/Hierarchy.h>

#include <xatlas.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>
#include <system_error>
#include <thread>

namespace shockwave {

namespace fs = std::filesystem;
using namespace bake;

// Bumped whenever the estimator, unwrap or rasterisation changes what a bake produces.
constexpr uint32_t kAlgorithm = 1;
constexpr const char* kTool = "shockwave lighting 1";

uint32_t LightingBakeSettings::effectiveSamples() const {
    if (samples) return samples;
    return quality == Quality::Final ? 512u : 64u;
}
const char* lightingQualityName(LightingBakeSettings::Quality quality) {
    return quality == LightingBakeSettings::Quality::Final ? "final" : "preview";
}
bool parseLightingQuality(const std::string& text, LightingBakeSettings::Quality& quality) {
    if (text == "preview")
        quality = LightingBakeSettings::Quality::Preview;
    else if (text == "final")
        quality = LightingBakeSettings::Quality::Final;
    else
        return false;
    return true;
}

void LightingBakeProgress::setStage(std::string stage) {
    std::lock_guard lock(m_mutex);
    m_stage = std::move(stage);
}
std::string LightingBakeProgress::stage() const {
    std::lock_guard lock(m_mutex);
    return m_stage;
}

std::string LightingBakeReport::text() const {
    std::ostringstream out;
    out << "manifest " << manifest << "\n";
    out << "receivers " << receivers << " in " << atlases << " atlases, " << texels << " texels ("
        << int(coverage * 100.0f + 0.5f) << "% of the atlases), " << samples << " paths a texel, "
        << (lightmapBytes + 1023) / 1024 << " KiB\n";
    out << "probe volumes " << volumes << ", probes " << probes << " (" << invalidProbes << " inside geometry), "
        << (probeBytes + 1023) / 1024 << " KiB\n";
    out << "took " << seconds << " s\n";
    for (const std::string& w : warnings) out << "warning: " << w << "\n";
    return out.str();
}

std::string lightingManifestPath(const World& world, const std::string& scenePath) {
    if (const Pool<BakedLighting>* pool = world.pool<BakedLighting>(); pool && pool->size())
        if (!pool->components()[0].asset.empty()) return pool->components()[0].asset;
    return "Lighting/" + fs::path(scenePath).stem().generic_string() + ".swlighting";
}

namespace {

// Runs fn(i) for i in 0..count on a few threads, stopping early when cancelled.
template <class Fn>
bool parallel(uint32_t count, uint32_t threads, LightingBakeProgress* progress, float from, float to, Fn&& fn) {
    std::atomic<uint32_t> next{0}, done{0};
    auto work = [&] {
        for (uint32_t i = next++; i < count; i = next++) {
            if (progress && progress->cancel.load()) return;
            fn(i);
            const uint32_t finished = ++done;
            if (progress) progress->fraction.store(from + (to - from) * float(finished) / float(std::max(count, 1u)));
        }
    };
    std::vector<std::thread> pool;
    for (uint32_t t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (std::thread& t : pool) t.join();
    return !(progress && progress->cancel.load());
}

// One receiver's charts, unwrapped on its own and placed in an atlas.
struct Unwrapped {
    const BakeSurface* surface = nullptr;
    LightingReceiver receiver;
    std::vector<glm::vec2> texels;   // per output vertex, in texels inside its own rectangle
    uint32_t width = 0, height = 0;
    uint32_t x = 0, y = 0;   // where the rectangle went
};

bool unwrap(Unwrapped& u, uint32_t padding, std::string& error) {
    const BakeSurface& s = *u.surface;
    const MeshData& mesh = *s.mesh;
    // World-space positions in texels: one unit is one texel, whatever the object's scale.
    std::vector<glm::vec3> positions(mesh.vertices.size()), normals(mesh.vertices.size());
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(s.toWorld)));
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        positions[i] = glm::vec3(s.toWorld * glm::vec4(mesh.vertices[i].position, 1.0f)) * s.texelsPerMeter;
        const glm::vec3 n = normalMatrix * mesh.vertices[i].normal;
        normals[i] = glm::length(n) > 1e-8f ? glm::normalize(n) : glm::vec3(0, 1, 0);
    }
    xatlas::Atlas* atlas = xatlas::Create();
    xatlas::MeshDecl decl;
    decl.vertexPositionData = positions.data();
    decl.vertexPositionStride = sizeof(glm::vec3);
    decl.vertexNormalData = normals.data();
    decl.vertexNormalStride = sizeof(glm::vec3);
    decl.vertexCount = uint32_t(positions.size());
    decl.indexData = mesh.indices.data();
    decl.indexCount = uint32_t(mesh.indices.size());
    decl.indexFormat = xatlas::IndexFormat::UInt32;
    const xatlas::AddMeshError added = xatlas::AddMesh(atlas, decl);
    if (added != xatlas::AddMeshError::Success) {
        error = std::string("couldn't unwrap the mesh: ") + xatlas::StringForEnum(added);
        xatlas::Destroy(atlas);
        return false;
    }
    xatlas::PackOptions pack;
    pack.padding = padding;
    pack.texelsPerUnit = 1.0f;
    pack.bilinear = true;
    xatlas::Generate(atlas, xatlas::ChartOptions(), pack);
    if (atlas->meshCount != 1 || atlas->width == 0 || atlas->height == 0) {
        error = "unwrapping made no charts";
        xatlas::Destroy(atlas);
        return false;
    }
    const xatlas::Mesh& out = atlas->meshes[0];
    u.width = atlas->width + 2 * padding;
    u.height = atlas->height + 2 * padding;
    u.receiver.binding = s.binding;
    u.receiver.part = s.part;
    u.receiver.geometryHash = s.geometryHash;
    for (uint32_t i = 0; i < out.vertexCount; ++i) {
        const xatlas::Vertex& v = out.vertexArray[i];
        u.receiver.sourceVertex.push_back(v.xref);
        u.texels.push_back({v.uv[0] + float(padding), v.uv[1] + float(padding)});
    }
    u.receiver.indices.assign(out.indexArray, out.indexArray + out.indexCount);
    xatlas::Destroy(atlas);
    return true;
}

// Shelf packing of the receivers' rectangles into atlasSize squares, tallest first. Each
// atlas then shrinks to the smallest power of two (64 at least) that holds its charts, so a
// small scene doesn't carry a mostly empty atlas.
std::vector<uint32_t> packAtlases(std::vector<Unwrapped>& items, uint32_t atlasSize) {
    std::vector<size_t> order(items.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return items[a].height > items[b].height; });
    uint32_t atlas = 0, x = 0, y = 0, shelf = 0;
    std::vector<uint32_t> extent;
    for (size_t index : order) {
        Unwrapped& u = items[index];
        if (x + u.width > atlasSize) {
            x = 0;
            y += shelf;
            shelf = 0;
        }
        if (y + u.height > atlasSize) {
            ++atlas;
            x = y = shelf = 0;
        }
        u.x = x;
        u.y = y;
        u.receiver.atlas = atlas;
        x += u.width;
        shelf = std::max(shelf, u.height);
        extent.resize(atlas + 1, 0);
        extent[atlas] = std::max({extent[atlas], x, y + u.height});
    }
    std::vector<uint32_t> sizes;
    for (uint32_t used : extent) {
        uint32_t size = 64;
        while (size < used && size < atlasSize) size *= 2;
        sizes.push_back(std::min(size, atlasSize));
    }
    return sizes;
}

// A texel of an atlas a receiver's triangle covers.
struct Texel {
    glm::vec3 position{0.0f}, normal{0.0f}, geometric{0.0f};
    int32_t owner = -1;   // index into the receivers
    bool exact = false;   // the texel centre is inside the triangle (not just near its edge)
};

void rasterise(const Unwrapped& u, int32_t owner, uint32_t size, std::vector<Texel>& texels) {
    const BakeSurface& s = *u.surface;
    const MeshData& mesh = *s.mesh;
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(s.toWorld)));
    const float handedness = glm::determinant(glm::mat3(s.toWorld)) < 0.0f ? -1.0f : 1.0f;
    const glm::vec2 corner(float(u.x), float(u.y));
    for (size_t t = 0; t + 2 < u.receiver.indices.size(); t += 3) {
        const uint32_t i[3] = {u.receiver.indices[t], u.receiver.indices[t + 1], u.receiver.indices[t + 2]};
        glm::vec2 p[3];
        glm::vec3 w[3], n[3];
        for (int k = 0; k < 3; ++k) {
            p[k] = u.texels[i[k]] + corner;
            const Vertex& v = mesh.vertices[u.receiver.sourceVertex[i[k]]];
            w[k] = glm::vec3(s.toWorld * glm::vec4(v.position, 1.0f));
            n[k] = normalMatrix * v.normal;
        }
        glm::vec3 geometric = glm::cross(w[1] - w[0], w[2] - w[0]);
        if (glm::length(geometric) < 1e-12f) continue;
        geometric = glm::normalize(geometric) * handedness;
        const float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);
        if (std::abs(area) < 1e-12f) continue;
        const int x0 = std::max(0, int(std::floor(std::min({p[0].x, p[1].x, p[2].x}) - 1.0f)));
        const int y0 = std::max(0, int(std::floor(std::min({p[0].y, p[1].y, p[2].y}) - 1.0f)));
        const int x1 = std::min(int(size) - 1, int(std::ceil(std::max({p[0].x, p[1].x, p[2].x}) + 1.0f)));
        const int y1 = std::min(int(size) - 1, int(std::ceil(std::max({p[0].y, p[1].y, p[2].y}) + 1.0f)));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const glm::vec2 c(float(x) + 0.5f, float(y) + 0.5f);
                float b1 = ((c.x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (c.y - p[0].y)) / area;
                float b2 = ((p[1].x - p[0].x) * (c.y - p[0].y) - (c.x - p[0].x) * (p[1].y - p[0].y)) / area;
                float b0 = 1.0f - b1 - b2;
                const bool inside = b0 >= -1e-5f && b1 >= -1e-5f && b2 >= -1e-5f;
                Texel& texel = texels[size_t(y) * size + size_t(x)];
                if (!inside) {
                    // Conservative: a texel the triangle only grazes still gets a sample, from
                    // the triangle's nearest point, unless a triangle covers its centre.
                    if (texel.owner >= 0) continue;
                    b0 = std::max(b0, 0.0f);
                    b1 = std::max(b1, 0.0f);
                    b2 = std::max(b2, 0.0f);
                    const float sum = b0 + b1 + b2;
                    b0 /= sum, b1 /= sum, b2 /= sum;
                    const glm::vec2 nearest = p[0] * b0 + p[1] * b1 + p[2] * b2;
                    if (glm::length(nearest - c) > 0.75f) continue;
                } else if (texel.exact) {
                    continue;
                }
                texel.owner = owner;
                texel.exact = inside;
                texel.position = w[0] * b0 + w[1] * b1 + w[2] * b2;
                glm::vec3 normal = n[0] * b0 + n[1] * b1 + n[2] * b2;
                normal = glm::length(normal) > 1e-8f ? glm::normalize(normal) : geometric;
                if (glm::dot(normal, geometric) <= 0.0f) normal = geometric;
                texel.normal = normal;
                texel.geometric = geometric;
            }
    }
}

bool writeFile(const fs::path& file, std::span<const uint8_t> bytes, std::string& error) {
    return writeLightingFileAtomic(file, bytes, error);
}

}   // namespace

bool bakeLighting(
    const World& world, const Assets& assets, const fs::path& contentRoot, const std::string& scenePath,
    const LightingBakeSettings& settings, LightingBakeReport& report, std::string& error,
    LightingBakeProgress* progress, bool planOnly) {
    const auto started = std::chrono::steady_clock::now();
    report = {};
    auto stage = [&](const char* text, float fraction) {
        if (!progress) return;
        progress->setStage(text);
        progress->fraction.store(fraction);
    };
    auto cancelled = [&] {
        if (progress && progress->cancel.load()) {
            error = "cancelled; the last bake is still in place";
            return true;
        }
        return false;
    };
    const uint32_t samples = settings.effectiveSamples();
    if (samples == 0 || samples > settings.maxSamples) {
        error = "samples must be 1.." + std::to_string(settings.maxSamples) + " (raise the maximum to go further)";
        return false;
    }
    if (settings.atlasSize < 64 || settings.atlasSize > 2048) {
        error = "atlas size must be 64..2048";
        return false;
    }
    if (settings.padding < 4) {
        error = "padding must be at least 4 texels, so charts never bleed into each other";
        return false;
    }
    if (settings.bounces > 16 || settings.probeRays < 16 || settings.probeRays > 65536) {
        error = "bounces must be 0..16 and probe rays 16..65536";
        return false;
    }
    const uint32_t threads = settings.threads ? settings.threads : std::max(1u, std::thread::hardware_concurrency());
    report.samples = samples;

    stage("gathering the scene", 0.0f);
    const BakeSceneDescription scene = describeBakeScene(world, assets, contentRoot);
    report.warnings = scene.warnings;
    std::vector<const BakeSurface*> receivers;
    for (const BakeSurface& s : scene.surfaces)
        if (s.receiver) receivers.push_back(&s);
    if (receivers.empty() && scene.volumes.empty()) {
        error = "nothing to bake: give static objects BakeGeometry (mobility static, receive_lightmap) or add a "
                "DiffuseProbeVolume";
        return false;
    }
    report.manifest = lightingManifestPath(world, scenePath);
    if (fs::path(report.manifest).extension() != ".swlighting" || report.manifest.find("..") != std::string::npos) {
        error = "BakedLighting asset must be a .swlighting file inside Content, not \"" + report.manifest + "\"";
        return false;
    }

    // Unwrap each receiver and place it.
    stage("unwrapping lightmap UVs", 0.02f);
    std::vector<Unwrapped> unwrapped(receivers.size());
    for (size_t i = 0; i < receivers.size(); ++i) {
        if (cancelled()) return false;
        unwrapped[i].surface = receivers[i];
        std::string problem;
        if (!unwrap(unwrapped[i], settings.padding, problem)) {
            error = "receiver " + formatBindingId(receivers[i]->binding) + ": " + problem;
            return false;
        }
        if (unwrapped[i].width > settings.atlasSize || unwrapped[i].height > settings.atlasSize) {
            error = "receiver " + formatBindingId(receivers[i]->binding) + " needs " +
                    std::to_string(unwrapped[i].width) + " x " + std::to_string(unwrapped[i].height) +
                    " texels, more than one " + std::to_string(settings.atlasSize) +
                    " atlas: lower its texels_per_meter or raise the atlas size";
            return false;
        }
        if (progress) progress->fraction.store(0.02f + 0.08f * float(i + 1) / float(receivers.size()));
    }
    const std::vector<uint32_t> atlasSizes = packAtlases(unwrapped, settings.atlasSize);
    const uint32_t atlasCount = uint32_t(atlasSizes.size());
    report.receivers = uint32_t(receivers.size());
    report.atlases = atlasCount;
    uint64_t atlasTexels = 0;
    for (uint32_t size : atlasSizes) atlasTexels += uint64_t(size) * size;
    report.lightmapBytes = atlasTexels * 8;
    if (report.lightmapBytes > settings.memoryBudget) {
        error = "the lightmaps would take " + std::to_string(report.lightmapBytes >> 20) + " MiB in " +
                std::to_string(atlasCount) + " atlases, over the " + std::to_string(settings.memoryBudget >> 20) +
                " MiB budget: lower texels_per_meter or raise the budget";
        return false;
    }
    // Probe grids, predicted before baking.
    struct VolumePlan {
        const BakeVolume* volume;
        glm::uvec3 counts;
        float spacing;
    };
    std::vector<VolumePlan> plans;
    for (const BakeVolume& v : scene.volumes) {
        VolumePlan plan{&v, glm::uvec3(1u), v.volume.spacing};
        float spacing = std::numeric_limits<float>::max();
        for (int a = 0; a < 3; ++a) {
            const float size = 2.0f * v.volume.halfExtents[a] * glm::length(glm::vec3(v.toWorld[a]));
            plan.counts[a] = uint32_t(std::max(1.0f, std::round(size / v.volume.spacing) + 1.0f));
            if (plan.counts[a] > 1) spacing = std::min(spacing, size / float(plan.counts[a] - 1));
        }
        plan.spacing = spacing == std::numeric_limits<float>::max() ? v.volume.spacing : spacing;
        const uint64_t count = uint64_t(plan.counts.x) * plan.counts.y * plan.counts.z;
        if (count > kMaxProbesPerVolume) {
            error = "probe volume " + formatBindingId(v.binding) + " would hold " + std::to_string(count) +
                    " probes, over " + std::to_string(kMaxProbesPerVolume) + ": raise its spacing or shrink it";
            return false;
        }
        report.probes += uint32_t(count);
        plans.push_back(plan);
    }
    report.volumes = uint32_t(plans.size());
    report.probeBytes = 8 + 4 + 108 + 4 + uint64_t(report.probes) * (4 + 108 + kProbeVisibilityTexels * 8);
    if (planOnly) {
        report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return true;
    }

    stage("building the ray tracing structure", 0.1f);
    const BakeScene tracer(scene);

    // Lightmaps: rasterise, trace each covered texel, then fill the gutters.
    std::vector<std::vector<float>> atlases(atlasCount);
    for (uint32_t a = 0; a < atlasCount; ++a) {
        const uint32_t size = atlasSizes[a];
        stage("tracing lightmaps", 0.1f + 0.7f * float(a) / float(atlasCount));
        std::vector<Texel> texels(size_t(size) * size);
        for (size_t i = 0; i < unwrapped.size(); ++i)
            if (unwrapped[i].receiver.atlas == a) rasterise(unwrapped[i], int32_t(i), size, texels);
        std::vector<float>& rgba = atlases[a];
        rgba.assign(size_t(size) * size * 4, 0.0f);
        const uint32_t rows = 8;
        const uint32_t tiles = (size + rows - 1) / rows;
        const float from = 0.1f + 0.7f * float(a) / float(atlasCount),
                    to = 0.1f + 0.7f * float(a + 1) / float(atlasCount);
        const bool finished = parallel(tiles, threads, progress, from, to, [&](uint32_t tile) {
            for (uint32_t y = tile * rows; y < std::min(size, (tile + 1) * rows); ++y)
                for (uint32_t x = 0; x < size; ++x) {
                    Texel& texel = texels[size_t(y) * size + x];
                    if (texel.owner < 0) continue;
                    const LightingReceiver& r = unwrapped[size_t(texel.owner)].receiver;
                    // A seed of the receiver, its texel and the bake's seed: the same bytes on every run.
                    const uint64_t key = mix64(r.binding.hi ^ mix64(r.binding.lo ^ mix64(uint64_t(r.part) << 32 | a))) ^
                                         mix64((uint64_t(y) << 32 | x) ^ (uint64_t(settings.seed) << 48));
                    Random random(key);
                    const float jitterU = random.next(), jitterV = random.next();
                    const glm::vec3 origin = BakeScene::offset(texel.position, texel.geometric);
                    glm::vec3 sum(0.0f);
                    uint32_t inside = 0;
                    const float texelSize = 1.0f / unwrapped[size_t(texel.owner)].surface->texelsPerMeter;
                    for (uint32_t s = 0; s < samples; ++s) {
                        // A shifted Hammersley set over the hemisphere, then random continuations.
                        const float u1 = std::fmod((float(s) + 0.5f) / float(samples) + jitterU, 1.0f);
                        const float u2 = std::fmod(radicalInverse(s) + jitterV, 1.0f);
                        const glm::vec3 direction = cosineDirection(texel.normal, u1, u2);
                        if (glm::dot(direction, texel.geometric) <= 0.0f) continue;   // below the surface: no light
                        float first = 0.0f;
                        bool backface = false;
                        sum += tracer.incoming(origin, direction, settings.bounces, random, &first, &backface);
                        if (backface && first < texelSize) ++inside;
                    }
                    // A texel whose rays mostly start inside other geometry (where a wall meets
                    // the floor) would leak the dark inside; its neighbours fill it instead.
                    if (inside * 4 > samples) {
                        texel.owner = -1;
                        continue;
                    }
                    float* out = &rgba[(size_t(y) * size + x) * 4];
                    const glm::vec3 b = sum / float(samples);
                    out[0] = b.r;
                    out[1] = b.g;
                    out[2] = b.b;
                    out[3] = 1.0f;
                }
        });
        if (!finished || cancelled()) {
            error = "cancelled; the last bake is still in place";
            return false;
        }
        for (const Texel& t : texels) report.texels += t.owner >= 0;
        // Fill each chart's gutter from its edge, a ring at a time, so bilinear filtering at
        // the chart's border reads its own light rather than black.
        for (uint32_t ring = 0; ring < settings.padding; ++ring) {
            std::vector<float> next = rgba;
            for (uint32_t y = 0; y < size; ++y)
                for (uint32_t x = 0; x < size; ++x) {
                    if (rgba[(size_t(y) * size + x) * 4 + 3] > 0.0f) continue;
                    glm::vec3 sum(0.0f);
                    float count = 0.0f;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = int(x) + dx, ny = int(y) + dy;
                            if (nx < 0 || ny < 0 || nx >= int(size) || ny >= int(size)) continue;
                            const float* n = &rgba[(size_t(ny) * size + size_t(nx)) * 4];
                            if (n[3] <= 0.0f) continue;
                            sum += glm::vec3(n[0], n[1], n[2]);
                            count += 1.0f;
                        }
                    if (count == 0.0f) continue;
                    float* out = &next[(size_t(y) * size + x) * 4];
                    out[0] = sum.r / count;
                    out[1] = sum.g / count;
                    out[2] = sum.b / count;
                    out[3] = 1.0f;
                }
            rgba = std::move(next);
        }
    }
    report.coverage = atlasTexels ? float(report.texels) / float(atlasTexels) : 0.0f;

    // Probes: each probe's incoming light projected onto spherical harmonics, its validity,
    // and its distance moments for visibility.
    stage("baking probes", 0.8f);
    LightingProbeSet probeSet;
    {
        constexpr uint32_t kEnvironmentDirections = 4096;
        Sh9 radiance{};
        for (uint32_t i = 0; i < kEnvironmentDirections; ++i) {
            const glm::vec3 d = sphereDirection(i, kEnvironmentDirections);
            const auto y = shBasis(d);
            const glm::vec3 l = scene.environment.radiance(d);
            for (size_t k = 0; k < 9; ++k) radiance[k] += l * y[k];
        }
        for (glm::vec3& c : radiance) c *= 4.0f * glm::pi<float>() / float(kEnvironmentDirections);
        probeSet.environment = shRadianceToIrradiance(radiance);
    }
    std::vector<glm::vec3> texelDirections(kProbeVisibilityTexels);
    for (uint32_t t = 0; t < kProbeVisibilityTexels; ++t)
        texelDirections[t] = octahedralDecode(
            glm::vec2(float(t % kProbeVisibilitySize) + 0.5f, float(t / kProbeVisibilitySize) + 0.5f) /
            float(kProbeVisibilitySize));
    for (size_t vi = 0; vi < plans.size(); ++vi) {
        const VolumePlan& plan = plans[vi];
        LightingProbeVolume volume;
        volume.binding = plan.volume->binding;
        volume.toWorld = plan.volume->toWorld;
        volume.halfExtents = plan.volume->volume.halfExtents;
        volume.counts = plan.counts;
        volume.blendDistance = std::max(plan.volume->volume.blendDistance, 0.0f);
        volume.priority = plan.volume->volume.priority;
        volume.maxDistance = 2.0f * plan.spacing;
        volume.probes.resize(size_t(plan.counts.x) * plan.counts.y * plan.counts.z);
        const uint32_t rays = settings.probeRays;
        const float from = 0.8f + 0.15f * float(vi) / float(plans.size());
        const float to = 0.8f + 0.15f * float(vi + 1) / float(plans.size());
        const bool finished =
            parallel(uint32_t(volume.probes.size()), threads, progress, from, to, [&](uint32_t index) {
                const uint32_t x = index % plan.counts.x, y = (index / plan.counts.x) % plan.counts.y,
                               z = index / (plan.counts.x * plan.counts.y);
                const glm::vec3 position = volume.probePosition(x, y, z);
                Random random(
                    mix64(volume.binding.hi ^ mix64(volume.binding.lo ^ index)) ^ (uint64_t(settings.seed) << 40));
                // The Fibonacci set turned at random per probe, so neighbours don't share artefacts.
                const glm::vec3 axis =
                    glm::normalize(glm::vec3(random.next(), random.next(), random.next()) - 0.5f + 1e-3f);
                const float angle = random.next() * glm::two_pi<float>();
                const glm::mat3 turn = glm::mat3(glm::rotate(glm::mat4(1.0f), angle, axis));
                Sh9 radiance{};
                uint32_t backfaces = 0;
                std::array<glm::vec3, kProbeVisibilityTexels> moments{};   // sum of w*d, w*d^2, w
                for (uint32_t r = 0; r < rays; ++r) {
                    const glm::vec3 d = turn * sphereDirection(r, rays);
                    float first = 0.0f;
                    bool backface = false;
                    const glm::vec3 l = tracer.incoming(position, d, settings.bounces, random, &first, &backface);
                    backfaces += backface;
                    const auto basis = shBasis(d);
                    for (size_t k = 0; k < 9; ++k) radiance[k] += l * basis[k];
                    const float distance = std::min(first, volume.maxDistance);
                    for (uint32_t t = 0; t < kProbeVisibilityTexels; ++t) {
                        const float c = glm::dot(texelDirections[t], d);
                        if (c <= 0.0f) continue;
                        const float w = std::pow(c, 8.0f);   // a narrow cosine lobe per texel
                        moments[t] += glm::vec3(w * distance, w * distance * distance, w);
                    }
                }
                LightingProbe& probe = volume.probes[index];
                for (glm::vec3& c : radiance) c *= 4.0f * glm::pi<float>() / float(rays);
                probe.sh = shRadianceToIrradiance(radiance);
                // A probe that mostly sees the backs of surfaces is inside something.
                probe.valid = backfaces * 4 < rays;
                for (uint32_t t = 0; t < kProbeVisibilityTexels; ++t)
                    probe.distance[t] = moments[t].z > 0.0f
                                            ? glm::vec2(moments[t].x, moments[t].y) / moments[t].z
                                            : glm::vec2(volume.maxDistance, volume.maxDistance * volume.maxDistance);
            });
        if (!finished || cancelled()) {
            error = "cancelled; the last bake is still in place";
            return false;
        }
        for (const LightingProbe& p : volume.probes) report.invalidProbes += !p.valid;
        if (report.invalidProbes == volume.probes.size() && !volume.probes.empty())
            report.warnings.push_back(
                "every probe of volume " + formatBindingId(volume.binding) + " is inside geometry; move or resize it");
        probeSet.volumes.push_back(std::move(volume));
    }

    // Write everything into a hidden sibling, check it reads back, then swap it in.
    stage("writing", 0.95f);
    const fs::path manifestFile = contentRoot / report.manifest;
    const fs::path relativeDir = fs::path(report.manifest).replace_extension();
    const fs::path finalDir = contentRoot / relativeDir;
    const fs::path tempDir = finalDir.parent_path() / ("." + finalDir.filename().string() + ".bake");
    const fs::path oldDir = finalDir.parent_path() / ("." + finalDir.filename().string() + ".old");
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);
    if (ec) {
        error = "can't make " + tempDir.generic_string() + ": " + ec.message();
        return false;
    }
    LightingManifest manifest;
    manifest.scene = scenePath;
    manifest.algorithm = kAlgorithm;
    manifest.quality = lightingQualityName(settings.quality);
    manifest.samples = samples;
    manifest.bounces = settings.bounces;
    manifest.seed = settings.seed;
    manifest.atlasSize = settings.atlasSize;
    manifest.probeRays = settings.probeRays;
    manifest.tool = kTool;
    manifest.lightingHash = lightingManifestHash(scene.lightingHash, manifest);
    manifest.geometryHash = scene.geometryHash;
    auto addResource = [&](LightingResource::Kind kind, const std::string& name, std::span<const uint8_t> bytes,
                           uint32_t w = 0, uint32_t h = 0) {
        LightingResource r;
        r.kind = kind;
        r.path = (relativeDir / name).generic_string();
        r.hash = hashLightingBytes(bytes);
        r.bytes = bytes.size();
        r.width = w;
        r.height = h;
        manifest.resources.push_back(r);
        return writeFile(tempDir / name, bytes, error);
    };
    for (uint32_t a = 0; a < atlasCount; ++a) {
        std::vector<uint8_t> ktx2;
        const uint32_t size = atlasSizes[a];
        if (!encodeLightmapKtx2(size, size, atlases[a], ktx2, error)) return false;
        char name[32];
        std::snprintf(name, sizeof(name), "Lightmap_%03u.ktx2", a);
        if (!addResource(LightingResource::Kind::Lightmap, name, ktx2, size, size)) return false;
    }
    std::vector<LightingReceiver> receiverRecords;
    for (Unwrapped& u : unwrapped) {
        for (size_t i = 0; i < u.texels.size(); ++i)
            u.receiver.lightmapUv.push_back(
                (u.texels[i] + glm::vec2(float(u.x), float(u.y))) / float(atlasSizes[u.receiver.atlas]));
        receiverRecords.push_back(u.receiver);
        manifest.receivers.push_back({u.receiver.binding, u.receiver.part, u.receiver.atlas, u.surface->hash});
    }
    std::sort(receiverRecords.begin(), receiverRecords.end(), [](const LightingReceiver& a, const LightingReceiver& b) {
        return a.binding != b.binding ? a.binding < b.binding : a.part < b.part;
    });
    if (!receiverRecords.empty()) {
        std::vector<uint8_t> bytes;
        writeLightingReceivers(receiverRecords, bytes);
        if (!addResource(LightingResource::Kind::Receivers, "Receivers.swbake", bytes)) return false;
    }
    {
        std::vector<uint8_t> bytes;
        writeLightingProbes(probeSet, bytes);
        report.probeBytes = bytes.size();
        if (!addResource(LightingResource::Kind::Probes, "Probes.swprobe", bytes)) return false;
    }
    for (const BakeVolume& v : scene.volumes) manifest.volumes.push_back({v.binding, 0, 0, v.hash});
    // Read back what was written before it replaces anything.
    for (const LightingResource& r : manifest.resources) {
        std::vector<uint8_t> bytes;
        if (!readLightingFile(tempDir / fs::path(r.path).filename(), bytes, error)) return false;
        if (hashLightingBytes(bytes) != r.hash) {
            error = r.path + " didn't read back as written";
            return false;
        }
    }
    const std::string manifestText = lightingManifestText(manifest);
    {
        LightingManifest check;
        if (!parseLightingManifest(manifestText, report.manifest, check, error)) return false;
    }
    if (cancelled()) {
        fs::remove_all(tempDir, ec);
        return false;
    }
    // Publish: the payload folder is swapped whole, then the manifest that vouches for it.
    fs::remove_all(oldDir, ec);
    const bool hadOld = fs::exists(finalDir, ec);
    if (hadOld) {
        fs::rename(finalDir, oldDir, ec);
        if (ec) {
            error = "can't move the last bake aside: " + ec.message();
            return false;
        }
    }
    fs::rename(tempDir, finalDir, ec);
    if (ec) {
        error = "can't publish the bake: " + ec.message();
        if (hadOld) fs::rename(oldDir, finalDir, ec);
        return false;
    }
    if (!writeLightingFileAtomic(
            manifestFile, std::span(reinterpret_cast<const uint8_t*>(manifestText.data()), manifestText.size()),
            error)) {
        fs::remove_all(finalDir, ec);
        if (hadOld) fs::rename(oldDir, finalDir, ec);
        return false;
    }
    fs::remove_all(oldDir, ec);
    report.lightmapBytes = 0;
    for (const LightingResource& r : manifest.resources)
        if (r.kind == LightingResource::Kind::Lightmap) report.lightmapBytes += r.bytes;
    report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    stage("done", 1.0f);
    return true;
}

}   // namespace shockwave
