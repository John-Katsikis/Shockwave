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

}   // namespace

void Player::graphicsReport() {
    const shockwave::Renderer& renderer = engine().renderer();
    const shockwave::RendererCapabilities caps = renderer.capabilities();
    const auto settings = renderer.effectiveSettings();
    const shockwave::RendererMemoryReport memory = renderer.memoryReport();

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
    for (const auto& s : settings)
        std::printf(
            "  %-16s %s -> %s%s%s\n", s.name.c_str(), s.requested.c_str(), s.effective.c_str(),
            s.reason.empty() ? "" : "  (", s.reason.empty() ? "" : (s.reason + ")").c_str());
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
             << ", \"effective\": " << quoted(s.effective) << ", \"reason\": " << quoted(s.reason) << "}";
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
         << "\n  },\n  \"unkeyed_draws\": " << renderer.unkeyedDraws() << ",\n  \"draws\": " << renderer.drawCount()
         << "\n}\n";
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
