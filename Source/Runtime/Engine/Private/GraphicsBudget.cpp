#include <Engine/GraphicsBudget.h>

#include <algorithm>
#include <cstring>

namespace shockwave {

bool budgetedMemoryCategory(GpuMemoryCategory category) {
    switch (category) {
    case GpuMemoryCategory::Bloom:
    case GpuMemoryCategory::AntiAliasing:
    case GpuMemoryCategory::SurfaceBuffers:
    case GpuMemoryCategory::AmbientOcclusion:
    case GpuMemoryCategory::TemporalHistory:
    case GpuMemoryCategory::ReflectionProbes:
    case GpuMemoryCategory::Hzb:
    case GpuMemoryCategory::Fog:
    case GpuMemoryCategory::Transmission:
    case GpuMemoryCategory::Skin: return true;
    default: return false;
    }
}

uint64_t budgetedMemoryBytes(const RendererMemoryReport& report) {
    uint64_t total = 0;
    for (size_t i = 0; i < report.gpu.size(); ++i)
        if (budgetedMemoryCategory(GpuMemoryCategory(i))) total += report.gpu[i].bytes;
    return total;
}

const std::vector<MemoryBudgetStep>& memoryBudgetSteps() {
    // Histories first (the largest, and FXAA still smooths edges), then effects only some
    // scenes use, then the ones most scenes show. docs/GraphicsImplementation.md 17.3.
    using C = GpuMemoryCategory;
    static const std::vector<MemoryBudgetStep> steps = {
        {"anti_aliasing", "fxaa", C::TemporalHistory},
        {"skin_quality", "off", C::Skin},
        {"transmission_quality", "off", C::Transmission},
        {"fog_quality", "analytic", C::Fog},
        {"ambient_occlusion", "low", C::AmbientOcclusion},
        {"ambient_occlusion", "off", C::AmbientOcclusion},
        {"reflection_probes", "false", C::ReflectionProbes},
        {"occlusion_culling", "off", C::Hzb},
        {"bloom_quality", "low", C::Bloom},
    };
    return steps;
}

namespace {
// Where a value sits in a setting's menu order (choices run low to high); -1 if it isn't
// one of them.
int rank(const GraphicsSettingInfo& info, const std::string& value) {
    for (size_t i = 0; i < info.choices.size(); ++i)
        if (value == info.choices[i]) return int(i);
    return -1;
}
}   // namespace

std::optional<MemoryBudgetStep>
nextMemoryBudgetStep(const GraphicsSettings& settings, const RendererMemoryReport& memory, GraphicsSettings& lowered) {
    for (const MemoryBudgetStep& step : memoryBudgetSteps()) {
        if (memory.gpu[size_t(step.memory)].bytes == 0) continue;
        const GraphicsSettingInfo* info = findGraphicsSetting(step.key);
        if (!info) continue;
        const std::string now = info->get(settings);
        // true/false menus list "true" first: on is above off either way.
        int current = rank(*info, now), target = rank(*info, step.value);
        if (now == "true" || now == "false") {
            current = now == "true" ? 1 : 0;
            target = std::strcmp(step.value, "true") == 0 ? 1 : 0;
        }
        if (current < 0 || target < 0 || current <= target) continue;
        lowered = settings;
        std::string error;
        if (!info->set(lowered, step.value, error)) continue;
        return step;
    }
    return std::nullopt;
}

}   // namespace shockwave
