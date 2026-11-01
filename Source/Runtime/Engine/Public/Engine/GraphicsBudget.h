#pragma once

// The graphics memory budget (G13, docs/GraphicsBudget.md): graphics_memory_budget_mb caps
// what optional effects may allocate. When a frame's effects come to more, the next
// effect in a fixed order is turned down, one step a frame, until they fit or nothing
// optional is left. The player's own settings are never changed: the steps are what the
// renderer runs, reported as fallbacks with this reason.

#include <Engine/GraphicsSettings.h>
#include <Renderer/RenderTypes.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace shockwave {

// What the budget counts: the targets and buffers of effects a setting can turn down
// (TAA history, ambient occlusion and the surface prepass, bloom, the depth pyramid,
// fog, glass and skin copies, reflection probes). Not the scene's own colour and depth,
// shadow maps (shadow_quality's own tiers), meshes, textures or per-frame data.
bool budgetedMemoryCategory(GpuMemoryCategory category);
uint64_t budgetedMemoryBytes(const RendererMemoryReport& report);

// One step down: a setting and the value it goes to. Steps run in the order listed by
// memoryBudgetSteps(); a step applies only when the setting is above that value.
struct MemoryBudgetStep {
    const char* key;   // .swgraphics key
    const char* value;   // what it's turned down to
    GpuMemoryCategory memory;   // where what it frees is counted
};
const std::vector<MemoryBudgetStep>& memoryBudgetSteps();
// The first step that lowers these settings and frees memory the report shows in use, with
// the settings lowered; none when nothing optional is left to turn down. An effect the
// scene isn't using (no glass, no skin) is never turned down: that would save nothing.
std::optional<MemoryBudgetStep>
nextMemoryBudgetStep(const GraphicsSettings& settings, const RendererMemoryReport& memory, GraphicsSettings& lowered);

}   // namespace shockwave
