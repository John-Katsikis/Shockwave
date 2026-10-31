// --graphics-report: what the GPU supports, each graphics setting as asked for and as it
// actually runs (and why they differ), and GPU memory by category. Taken after a few
// frames, so effects that size themselves on the first frame are counted.

#include "Player.h"

#include <Renderer/Renderer.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace player {

namespace {

std::string quoted(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out + "\"";
}

double mib(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

// Motion vectors and TAA (G09, G10) in the last frame.
std::string temporalJson(const shockwave::TemporalStats& t) {
    char text[320];
    std::snprintf(
        text, sizeof(text),
        "{\"motion\": %s, \"taa\": %s, \"reset\": \"%s\", \"resets\": %u, \"history_draws\": %u, "
        "\"new_draws\": %u, \"jitter\": [%.4f, %.4f]}",
        t.motion ? "true" : "false", t.taa ? "true" : "false", shockwave::historyResetName(t.reset), t.resets,
        t.historyDraws, t.newDraws, t.jitter.x, t.jitter.y);
    return text;
}

// Clustered local lighting (G11) in the last frame; the GPU's counts are a frame or two late.
std::string clustersJson(const shockwave::ClusterStats& c) {
    char text[512];
    std::snprintf(
        text, sizeof(text),
        "{\"requested\": \"%s\", \"clustered\": %s, \"reason\": \"%s\", \"tile_size\": %u, "
        "\"grid\": [%u, %u, %u], \"cluster_far\": %g, \"local_lights\": %u, \"directional_lights\": %u, "
        "\"list_bytes\": %llu, \"max_candidates\": %u, \"average_candidates\": %.3f, \"overflow_clusters\": %u}",
        shockwave::lightCullingName(c.requested), c.clustered ? "true" : "false", c.reason, c.grid.tileSize,
        c.grid.tilesX, c.grid.tilesY, c.grid.slices, double(c.farPlane), c.localLights, c.directionalLights,
        static_cast<unsigned long long>(c.listBytes), c.maxCandidates, double(c.averageCandidates), c.overflowClusters);
    return text;
}

// Particles (A04) in the last frame.
std::string particlesJson(const shockwave::ParticleSettings& s, const shockwave::ParticleStats& p) {
    char text[512];
    std::snprintf(
        text, sizeof(text),
        "{\"soft_particles\": %s, \"distortion\": %s, \"lighting\": \"%s\", \"draws\": %u, \"vertices\": %u, "
        "\"soft_draws\": %u, \"distortion_draws\": %u, \"skipped_distortion\": %u, \"scene_lit_draws\": %u, "
        "\"depth_pass\": %s}",
        s.softParticles ? "true" : "false", s.distortion ? "true" : "false",
        shockwave::particleLightingName(s.lighting), p.draws, p.vertices, p.softDraws, p.distortionDraws,
        p.skippedDistortion, p.sceneLitDraws, p.depthPass ? "true" : "false");
    return text;
}

// Colour grading, exposure and fog (A05, A06) in the last frame; metering is a frame or two late.
std::string postProcessJson(const shockwave::PostProcessSettings& s, const shockwave::PostProcessStats& p) {
    char text[768];
    std::snprintf(
        text, sizeof(text),
        "{\"color_grading\": %s, \"auto_exposure\": %s, \"white_balance\": %s, \"luts\": %u, "
        "\"luts_approximated\": %s, \"exposure\": %.4f, \"auto_share\": %.4f, \"metered_valid\": %s, "
        "\"metered_target\": %.4f, \"clamped_fraction\": %.4f, \"exposure_resets\": %u, "
        "\"fog_requested\": \"%s\", \"fog_quality\": \"%s\", \"fog_reason\": \"%s\", "
        "\"froxels\": [%u, %u, %u], \"froxel_tile\": %u, \"fog_bytes\": %llu, \"exposure_bytes\": %llu}",
        s.colorGrading ? "true" : "false", s.autoExposure ? "true" : "false", p.whiteBalance ? "true" : "false", p.luts,
        p.lutsApproximated ? "true" : "false", double(p.exposure), double(p.autoExposure),
        p.meteredValid ? "true" : "false", double(p.meteredTarget), double(p.clampedFraction), p.exposureResets,
        shockwave::fogQualityName(p.fogRequested), shockwave::fogQualityName(p.fogQuality), p.fogReason,
        p.froxels.tilesX, p.froxels.tilesY, p.froxels.slices, p.froxels.tileSize,
        static_cast<unsigned long long>(p.fogBytes), static_cast<unsigned long long>(p.exposureBytes));
    return text;
}

}   // namespace

std::string graphicsInfoJson(const shockwave::GraphicsSettings& settings, const fs::path& userFile) {
    const auto preset = shockwave::matchingGraphicsPreset(settings);
    std::ostringstream json;
    json << "{\n  \"user_file\": " << quoted(userFile.string())
         << ",\n  \"preset\": " << quoted(preset ? shockwave::graphicsPresetName(*preset) : "custom")
         << ",\n  \"settings\": {";
    const auto& info = shockwave::graphicsSettingInfo();
    for (size_t i = 0; i < info.size(); ++i)
        json << (i ? ", " : "") << quoted(info[i].key) << ": " << quoted(info[i].get(settings));
    json << "},\n  \"descriptors\": [";
    for (size_t i = 0; i < info.size(); ++i) {
        const auto& d = info[i];
        json << (i ? ",\n" : "\n") << "    {\"key\": " << quoted(d.key) << ", \"label\": " << quoted(d.label)
             << ", \"category\": " << quoted(d.category) << ", \"values\": " << quoted(d.values)
             << ", \"applies\": " << quoted(shockwave::graphicsApplyName(d.apply))
             << ", \"preset\": " << (d.preset ? "true" : "false") << ", \"tooltip\": " << quoted(d.tooltip) << "}";
    }
    json << "\n  ]\n}\n";
    return json.str();
}

void Player::graphicsReport() {
    const shockwave::Renderer& renderer = engine().renderer();
    const shockwave::RendererCapabilities caps = renderer.capabilities();
    const auto settings = renderer.effectiveSettings();
    const shockwave::RendererMemoryReport memory = renderer.memoryReport();
    const shockwave::TextureResidencyStats residency = renderer.textureResidency();

    std::printf("renderer: %s\n", caps.backend.c_str());
    if (!caps.device.empty())
        std::printf("device: %s\ndriver: %s\napi: %s\n", caps.device.c_str(), caps.driver.c_str(), caps.api.c_str());
    std::printf("\ncapabilities:\n");
    for (const auto& f : caps.features)
        std::printf(
            "  %-34s %s%s%s\n", f.name.c_str(), f.supported ? "yes" : "no", f.detail.empty() ? "" : "  ",
            f.detail.c_str());
    std::printf("\nlimits:\n");
    for (const auto& l : caps.limits)
        std::printf("  %-34s %llu\n", l.name.c_str(), static_cast<unsigned long long>(l.value));
    std::printf("\nsettings (requested -> effective):\n");
    for (const auto& s : settings) {
        std::printf(
            "  %-16s %s -> %s%s%s\n", s.name.c_str(), s.requested.c_str(), s.effective.c_str(),
            s.reason.empty() ? "" : "  (", s.reason.empty() ? "" : (s.reason + ")").c_str());
        if (!s.detail.empty()) std::printf("  %-16s   %s\n", "", s.detail.c_str());
    }
    std::printf("\ngpu memory:\n");
    for (size_t i = 0; i < memory.gpu.size(); ++i) {
        const auto& usage = memory.gpu[i];
        if (!usage.allocations) continue;
        std::printf(
            "  %-20s %9.2f MiB  %u allocation%s\n", shockwave::gpuMemoryCategoryName(shockwave::GpuMemoryCategory(i)),
            mib(usage.bytes), usage.allocations, usage.allocations == 1 ? "" : "s");
    }
    std::printf("  %-20s %9.2f MiB\n", "total", mib(memory.gpuBytes()));
    std::printf("  %-20s %9.2f MiB\n", "pipeline cache", mib(memory.pipelineCacheBytes));
    if (memory.deviceBudget)
        std::printf(
            "  %-20s %9.2f of %.2f MiB (the driver's count, device-local heaps)\n", "device usage",
            mib(memory.deviceUsage), mib(memory.deviceBudget));
    std::printf("  %-20s %9.2f MiB (CPU copies for swapping the renderer)\n", "cpu copies", mib(memory.cpuCopyBytes));
    std::printf(
        "\ntexture quality: %u of %u material textures reduced, %u pending; %.2f MiB on the GPU (%.2f MiB at "
        "ultra)\n",
        residency.reduced, residency.textures, residency.pending, mib(residency.residentBytes),
        mib(residency.fullBytes));

    if (m_tool.benchmarkJson.empty()) return;
    std::ostringstream json;
    json << "{\n  \"renderer\": " << quoted(caps.backend) << ",\n  \"device\": " << quoted(caps.device)
         << ",\n  \"driver\": " << quoted(caps.driver) << ",\n  \"api\": " << quoted(caps.api)
         << ",\n  \"capabilities\": [";
    for (size_t i = 0; i < caps.features.size(); ++i) {
        const auto& f = caps.features[i];
        json << (i ? ",\n" : "\n") << "    {\"name\": " << quoted(f.name)
             << ", \"supported\": " << (f.supported ? "true" : "false") << ", \"detail\": " << quoted(f.detail) << "}";
    }
    json << "\n  ],\n  \"limits\": {";
    for (size_t i = 0; i < caps.limits.size(); ++i)
        json << (i ? ", " : "") << quoted(caps.limits[i].name) << ": " << caps.limits[i].value;
    json << "},\n  \"settings\": [";
    for (size_t i = 0; i < settings.size(); ++i) {
        const auto& s = settings[i];
        json << (i ? ",\n" : "\n") << "    {\"name\": " << quoted(s.name) << ", \"requested\": " << quoted(s.requested)
             << ", \"effective\": " << quoted(s.effective) << ", \"reason\": " << quoted(s.reason)
             << ", \"detail\": " << quoted(s.detail) << "}";
    }
    json << "\n  ],\n  \"memory\": {\n    \"categories\": {";
    for (size_t i = 0; i < memory.gpu.size(); ++i)
        json << (i ? ", " : "") << quoted(shockwave::gpuMemoryCategoryName(shockwave::GpuMemoryCategory(i)))
             << ": {\"bytes\": " << memory.gpu[i].bytes << ", \"allocations\": " << memory.gpu[i].allocations << "}";
    json << "},\n    \"gpu_bytes\": " << memory.gpuBytes()
         << ",\n    \"pipeline_cache_bytes\": " << memory.pipelineCacheBytes
         << ",\n    \"device_usage_bytes\": " << memory.deviceUsage
         << ",\n    \"device_budget_bytes\": " << memory.deviceBudget
         << ",\n    \"cpu_copy_bytes\": " << memory.cpuCopyBytes
         << "\n  },\n  \"texture_residency\": {\"material_textures\": " << residency.textures
         << ", \"reduced\": " << residency.reduced << ", \"pending\": " << residency.pending
         << ", \"resident_bytes\": " << residency.residentBytes << ", \"full_bytes\": " << residency.fullBytes
         << ", \"retained_cpu_bytes\": " << residency.retainedBytes
         << "},\n  \"temporal\": " << temporalJson(renderer.temporalStats())
         << ",\n  \"clusters\": " << clustersJson(renderer.clusterStats())
         << ",\n  \"particles\": " << particlesJson(renderer.particleSettings(), renderer.particleStats())
         << ",\n  \"postprocess\": " << postProcessJson(renderer.postProcessSettings(), renderer.postProcessStats())
         << ",\n  \"unkeyed_draws\": " << renderer.unkeyedDraws() << ",\n  \"draws\": " << renderer.drawCount()
         << ",\n  \"frame_graph\": " << quoted(renderer.frameGraph()) << "\n}\n";
    std::ofstream out(m_tool.benchmarkJson, std::ios::trunc);
    out << json.str();
    out.close();
    if (!out) {
        std::fprintf(stderr, "Player: can't write %s\n", m_tool.benchmarkJson.string().c_str());
        m_exitCode = 1;
        return;
    }
    std::printf("Wrote %s\n", m_tool.benchmarkJson.string().c_str());
}

}   // namespace player
