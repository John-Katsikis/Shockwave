// Colour grading, exposure and fog in the editor (A05, A06, docs/PostProcess.md): Create >
// Postprocess volume, the Inspector's profile picker with its problems, and the selected
// volume's box (its blend distance drawn inside) in the viewport. Profiles reload by
// themselves when their files change, so tuning one shows straight away.
#include "Editor.h"

#include <Engine/PostProcess.h>
#include <UI/Gui.h>

#include <algorithm>
#include <cmath>

namespace editor {

namespace gui = shockwave::gui;
using shockwave::Entity;

Entity Editor::createPostProcessVolume() {
    pushUndo(m_world);
    const Entity e = m_world.create();
    m_world.add<shockwave::Name>(e, {uniqueName("Postprocess volume")});
    m_world.add<shockwave::Transform>(e, {placementPoint() + glm::vec3(0.0f, 2.5f, 0.0f)});
    shockwave::PostProcessVolume volume;
    if (m_postProcess) {
        const auto files = m_postProcess->profileFiles();
        if (!files.empty()) volume.profile = files.front();
    }
    m_world.add<shockwave::PostProcessVolume>(e, volume);
    select(e);
    log("Created " + displayName(e) +
        (volume.profile.empty() ? std::string(". Pick a .swpostprocess profile in the Inspector (docs/PostProcess.md)")
                                : " using " + volume.profile));
    return e;
}

void Editor::drawPostProcessVolumeNotes(Entity e) {
    auto* volume = m_world.get<shockwave::PostProcessVolume>(e);
    if (!volume || !m_postProcess) return;
    const std::vector<std::string> files = m_postProcess->profileFiles();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(
            "##profile", volume->profile.empty() ? "Pick a profile..." : volume->profile.c_str(),
            ImGuiComboFlags_HeightLarge)) {
        if (files.empty()) ImGui::TextDisabled("No .swpostprocess files in Content yet");
        for (const std::string& file : files) {
            if (ImGui::Selectable(file.c_str(), file == volume->profile) && file != volume->profile) {
                pushUndo(m_world);
                volume->profile = file;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("A .swpostprocess file in Content: exposure, white balance, a LUT, bloom and fog");
    if (volume->profile.empty()) return;
    std::string error;
    const auto profile = m_postProcess->profile(volume->profile, &error);
    if (profile && !profile->values.lut.empty() && !m_postProcess->lut(profile->values.lut, &error))
        error = "lut " + profile->values.lut + ": " + error;
    if (!error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, gui::semanticColor(gui::currentTheme().error));
        ImGui::TextWrapped("%s", error.c_str());
        ImGui::PopStyleColor();
        return;
    }
    if (profile->chain.size() > 1) ImGui::TextDisabled("Inherits from %s", profile->chain[1].c_str());
    const shockwave::Transform* t = m_world.get<shockwave::Transform>(e);
    const glm::vec3 camera = glm::vec3(glm::inverse(engine().renderer().cameraView())[3]);
    const float here = volume->unbounded || !t
                           ? (volume->enabled ? std::clamp(volume->weight, 0.0f, 1.0f) : 0.0f)
                           : shockwave::postProcessVolumeWeight(*volume, shockwave::worldMatrix(m_world, e), camera);
    ImGui::TextDisabled("Applies %.0f%% at the viewport camera", static_cast<double>(here * 100.0f));
}

// A selected volume: its box, and the inner box where its blend has fully faded in.
void Editor::drawPostProcessVolume(Entity e, const ImVec2& pos, const ImVec2& size, ImU32 color) {
    const auto* volume = m_world.get<shockwave::PostProcessVolume>(e);
    if (!volume || volume->unbounded || !m_world.has<shockwave::Transform>(e)) return;
    const glm::mat4 model = shockwave::worldMatrix(m_world, e);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    auto box = [&](const glm::vec3& h, float thickness, ImU32 c) {
        ImVec2 screen[8];
        bool visible[8];
        for (int i = 0; i < 8; ++i)
            visible[i] = worldToScreen(
                glm::vec3(model * glm::vec4((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z, 1.0f)),
                pos, size, screen[i]);
        for (int i = 0; i < 8; ++i)
            for (int bit = 1; bit < 8; bit <<= 1) {
                const int j = i | bit;
                if (j != i && visible[i] && visible[j]) draw->AddLine(screen[i], screen[j], c, thickness);
            }
    };
    box(volume->halfExtents, 2.0f, color);
    if (volume->blendDistance > 0.0f) {
        // The blend is in world metres: undo the scale per axis.
        const glm::vec3 scale(
            glm::length(glm::vec3(model[0])), glm::length(glm::vec3(model[1])), glm::length(glm::vec3(model[2])));
        const glm::vec3 inner =
            glm::max(volume->halfExtents - volume->blendDistance / glm::max(scale, glm::vec3(1e-6f)), glm::vec3(0.0f));
        if (inner.x > 0.0f && inner.y > 0.0f && inner.z > 0.0f) box(inner, 1.0f, (color & 0x00FFFFFFu) | 0x80000000u);
    }
}

}   // namespace editor
