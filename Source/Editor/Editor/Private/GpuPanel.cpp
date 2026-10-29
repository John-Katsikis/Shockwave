#include "GpuPanel.h"

#include "Preferences.h"

#include <Engine/Engine.h>
#include <Renderer/GpuSelection.h>
#include <Renderer/Renderer.h>
#include <UI/Gui.h>

#include <imgui.h>

#include <cinttypes>
#include <string>

namespace gui = shockwave::gui;

namespace editor {

void drawGpuStats(const shockwave::Renderer& renderer) {
    const shockwave::GpuChoice* choice = renderer.gpuChoice();
    if (!choice) return;
    const ImVec4 warning = gui::semanticColor(gui::currentTheme().warning);
    for (const std::string& text : choice->warnings) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(warning, "%s", text.c_str());
        ImGui::PopTextWrapPos();
    }
    if (!ImGui::TreeNode("GPUs")) return;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(choice->reason.c_str());
    ImGui::PopTextWrapPos();
    for (size_t i = 0; i < choice->gpus.size(); ++i) {
        const shockwave::GpuCandidate& gpu = choice->gpus[i];
        const bool chosen = static_cast<int>(i) == choice->chosen;
        if (chosen)
            ImGui::Text("%zu  %s  (in use)", i, gpu.name.c_str());
        else
            ImGui::TextDisabled("%zu  %s", i, gpu.name.c_str());
        ImGui::SetItemTooltip(
            "%s, %s\nVulkan %s, driver %s\n%llu MB%s%s%s", shockwave::gpuKindName(gpu.kind),
            shockwave::gpuVendorName(gpu.vendorId), gpu.vulkanVersion.c_str(), gpu.driver.c_str(),
            static_cast<unsigned long long>(gpu.memoryMB),
            gpu.display == shockwave::GpuDisplay::Yes ? "\nYour screen is plugged into it" : "",
            gpu.problem.empty() ? "" : "\nCan't run Shockwave: ", gpu.problem.c_str());
    }
    ImGui::TextDisabled("player --gpus prints this report");
    ImGui::TreePop();
}

bool drawGpuPreference(shockwave::Engine& engine, Preferences& prefs) {
    const shockwave::GpuChoice* choice = engine.renderer().gpuChoice();
    if (!choice) return false;   // a renderer without a GPU
    bool changed = false;
    const std::string preview = prefs.gpu.empty() ? std::string("Automatic") : prefs.gpu;
    if (ImGui::BeginCombo("GPU", preview.c_str())) {
        auto pick = [&](const std::string& request) {
            prefs.gpu = request;
            shockwave::setRequestedGpu(request);
            engine.reloadRenderer();   // starts again on the new GPU; meshes and textures are kept
            changed = true;
        };
        if (ImGui::Selectable("Automatic", prefs.gpu.empty()) && !prefs.gpu.empty()) pick("");
        ImGui::SetItemTooltip("Let Shockwave pick: dedicated GPUs first, then the one your screen is plugged into");
        for (const shockwave::GpuCandidate& gpu : choice->gpus) {
            ImGui::BeginDisabled(!gpu.problem.empty());
            if (ImGui::Selectable(gpu.name.c_str(), prefs.gpu == gpu.name) && prefs.gpu != gpu.name) pick(gpu.name);
            ImGui::EndDisabled();
            if (!gpu.problem.empty())
                ImGui::SetItemTooltip("Can't run Shockwave: %s", gpu.problem.c_str());
            else if (gpu.display == shockwave::GpuDisplay::Yes)
                ImGui::SetItemTooltip("Your screen is plugged into this one");
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The GPU the editor renders on; switching restarts the renderer.\n"
                          "The player takes --gpu NAME, or the SHOCKWAVE_GPU environment variable");
    return changed;
}

void drawGraphicsReport(const shockwave::Renderer& renderer) {
    const ImVec4 warning = gui::semanticColor(gui::currentTheme().warning);
    // Settings first, open while any of them fell back: that's what someone needs to see.
    const auto settings = renderer.effectiveSettings();
    bool fallback = false;
    for (const auto& s : settings) fallback = fallback || !s.reason.empty();
    if (fallback) ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
    if (ImGui::TreeNode("Graphics settings")) {
        if (gui::beginPropertyGrid("effective settings")) {
            for (const auto& s : settings) {
                gui::propertyLabel(s.name.c_str());
                if (s.reason.empty()) {
                    ImGui::TextUnformatted(s.effective.c_str());
                } else {
                    ImGui::TextColored(warning, "%s (asked for %s)", s.effective.c_str(), s.requested.c_str());
                    ImGui::SetItemTooltip("%s", s.reason.c_str());
                }
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("GPU capabilities")) {
        const shockwave::RendererCapabilities caps = renderer.capabilities();
        if (!caps.api.empty()) ImGui::TextDisabled("%s, %s", caps.api.c_str(), caps.driver.c_str());
        for (const auto& f : caps.features) {
            if (f.supported)
                ImGui::BulletText("%s", f.name.c_str());
            else
                ImGui::TextColored(warning, "  missing: %s", f.name.c_str());
            if (!f.detail.empty()) ImGui::SetItemTooltip("%s", f.detail.c_str());
        }
        if (!caps.limits.empty() && ImGui::TreeNode("Limits")) {
            for (const auto& l : caps.limits) ImGui::Text("%-28s %" PRIu64, l.name.c_str(), l.value);
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("GPU memory")) {
        // Walks every allocation, so only while this is open.
        const shockwave::RendererMemoryReport memory = renderer.memoryReport();
        auto mib = [](uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };
        if (gui::beginPropertyGrid("gpu memory")) {
            for (size_t i = 0; i < memory.gpu.size(); ++i) {
                if (!memory.gpu[i].allocations) continue;
                gui::propertyLabel(shockwave::gpuMemoryCategoryName(shockwave::GpuMemoryCategory(i)));
                ImGui::Text("%.2f MiB", mib(memory.gpu[i].bytes));
                ImGui::SetItemTooltip("%u allocations", memory.gpu[i].allocations);
            }
            gui::propertyLabel("Total");
            ImGui::Text("%.2f MiB", mib(memory.gpuBytes()));
            if (memory.deviceBudget) {
                gui::propertyLabel("Device (driver)");
                ImGui::Text("%.0f of %.0f MiB", mib(memory.deviceUsage), mib(memory.deviceBudget));
                ImGui::SetItemTooltip("Everything this process has on device-local heaps, as the driver counts it");
            }
            gui::propertyLabel("CPU copies");
            ImGui::Text("%.2f MiB", mib(memory.cpuCopyBytes));
            ImGui::SetItemTooltip("Meshes and textures kept on the CPU so the renderer can be swapped");
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
}

}   // namespace editor
