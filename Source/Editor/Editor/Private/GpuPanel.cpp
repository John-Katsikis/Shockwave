#include "GpuPanel.h"

#include "Preferences.h"

#include <Engine/Engine.h>
#include <Renderer/GpuSelection.h>
#include <UI/Gui.h>

#include <imgui.h>

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

}   // namespace editor
