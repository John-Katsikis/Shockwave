#pragma once

// The offline lighting baker (G05, G06; docs/Lighting.md). Bakes a scene's static diffuse
// lighting into lightmaps and its probe volumes into spherical harmonics, on the CPU, and
// publishes the result atomically: a failed or cancelled bake leaves the last good one.
//
//   LightingBakeSettings settings;   // preview: 64 paths a texel
//   LightingBakeReport report;
//   if (!bakeLighting(world, assets, contentRoot, "Scenes/Arena.swscene", settings, report, error)) ...
//
// The estimator (docs/GraphicsImplementation.md 9.7): per lightmap texel, cosine-weighted
// paths into its hemisphere. A path that leaves the scene brings back the environment;
// one that hits a static surface brings its emission and, while it may still reflect,
// that surface's diffuse reflectance times the baked lights reaching it (with visibility)
// plus one continued path. The receiver's own direct light is never baked: it stays
// real-time. Seeds come from binding, texel and sample, never from thread timing, so the
// same scene and seed bake the same bytes.

#include <Engine/BakedLighting.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace shockwave {

struct LightingBakeSettings {
    enum class Quality { Preview, Final };
    Quality quality = Quality::Preview;
    uint32_t samples = 0;   // paths per texel; 0: 64 for preview, 512 for final
    uint32_t maxSamples = 8192;   // samples above this are refused
    uint32_t bounces = 2;   // diffuse reflections a path may make (0: environment and emission only)
    uint32_t seed = 1;
    uint32_t atlasSize = 1024;   // atlas width and height, 64..2048
    uint32_t padding = 4;   // texels between charts, at least 4
    uint32_t probeRays = 256;   // per probe
    uint64_t memoryBudget = 256ull << 20;   // predicted lightmap bytes above this are refused
    uint32_t threads = 0;   // 0: one per core
    uint32_t effectiveSamples() const;
};
const char* lightingQualityName(LightingBakeSettings::Quality quality);   // "preview", "final"
bool parseLightingQuality(const std::string& text, LightingBakeSettings::Quality& quality);

// Shared with the editor's Bake window: read from any thread, cancel from any thread.
struct LightingBakeProgress {
    std::atomic<float> fraction{0.0f};   // 0..1
    std::atomic<bool> cancel{false};   // ask the bake to stop; it fails with "cancelled"
    void setStage(std::string stage);
    std::string stage() const;

private:
    mutable std::mutex m_mutex;
    std::string m_stage;
};

struct LightingBakeReport {
    std::string manifest;   // inside Content: "Lighting/Arena.swlighting"
    uint32_t receivers = 0, atlases = 0;
    uint64_t texels = 0;   // lightmap texels covered by charts
    float coverage = 0;   // of all atlas texels
    uint64_t lightmapBytes = 0;   // on disk and on the GPU
    uint32_t volumes = 0, probes = 0, invalidProbes = 0;
    uint64_t probeBytes = 0;
    uint32_t samples = 0;
    double seconds = 0;
    std::vector<std::string> warnings;
    std::string text() const;   // a few readable lines
};

// Bakes the scene (scenePath names it inside Content, for the manifest) and publishes it at
// the scene's BakedLighting asset, or Lighting/<scene name>.swlighting without one. CPU only.
// planOnly unwraps and checks budgets, then stops before tracing and writes nothing.
bool bakeLighting(
    const World& world, const Assets& assets, const std::filesystem::path& contentRoot, const std::string& scenePath,
    const LightingBakeSettings& settings, LightingBakeReport& report, std::string& error,
    LightingBakeProgress* progress = nullptr, bool planOnly = false);
// Where a scene's bake goes: its BakedLighting asset, or Lighting/<stem>.swlighting.
std::string lightingManifestPath(const World& world, const std::string& scenePath);

}   // namespace shockwave
