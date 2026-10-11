// Skeletal animation in the editor: Animators pose their models in the viewport while
// editing (at their time, or playing with View > Preview animations), the Inspector picks
// clips and scrubs through them, the viewport can draw the selected model's skeleton, and
// Stats shows what animation costs. See docs/Animation.md.
#include "Editor.h"

#include <Animation/Animation.h>
#include <Animation/AnimationWorld.h>
#include <Engine/Hierarchy.h>
#include <Engine/Pose.h>
#include <UI/Gui.h>

#include <algorithm>

namespace editor {

namespace gui = shockwave::gui;
using shockwave::Animator;
using shockwave::Entity;

void Editor::updateAnimation(double dt) {
    if (!m_animation) return;
    // During Play the session's AnimationWorld runs the game's Animators.
    if (playing()) return;
    m_animation->update(m_previewAnimation ? dt : 0.0);
}

void Editor::restartAnimation() {
    if (m_animation) m_animation->restart();
}

void Editor::addAnimatorIfAnimated(Entity e) {
    const auto* renderer = m_world.get<shockwave::ModelRenderer>(e);
    const auto asset = renderer ? engine().assets().model(renderer->model) : nullptr;
    if (!asset || asset->data.animations.empty() || m_world.has<Animator>(e)) return;
    m_world.add<Animator>(e);
}

// Under a ModelRenderer whose model has clips but nothing playing them.
void Editor::drawModelAnimationNotes(Entity e) {
    const auto* renderer = m_world.get<shockwave::ModelRenderer>(e);
    const auto asset = renderer ? engine().assets().model(renderer->model) : nullptr;
    if (!asset || asset->data.animations.empty() || m_world.has<Animator>(e)) return;
    ImGui::TextDisabled(
        "%zu animation clip%s. Add an Animator to play them.", asset->data.animations.size(),
        asset->data.animations.size() == 1 ? "" : "s");
    if (gui::button("Add Animator")) {
        pushUndo(m_world);
        m_world.add<Animator>(e);
    }
}

void Editor::drawAnimatorNotes(Entity e) {
    for (const auto& problem : shockwave::validateAnimator(m_world, e, engine().assets())) {
        const auto& theme = gui::currentTheme();
        ImGui::PushStyleColor(ImGuiCol_Text, gui::semanticColor(problem.warning ? theme.warning : theme.error));
        ImGui::TextWrapped("%s", problem.message.c_str());
        ImGui::PopStyleColor();
    }
    Animator* animator = m_world.get<Animator>(e);
    const auto* renderer = m_world.get<shockwave::ModelRenderer>(e);
    const auto asset = renderer ? engine().assets().model(renderer->model) : nullptr;
    if (!animator || !asset || asset->data.animations.empty()) return;
    const auto& clips = asset->data.animations;

    // The clip, from the model's list rather than typed.
    const shockwave::ModelAnimation* clip =
        animator->clip.empty() ? &clips.front() : shockwave::findAnimation(asset->data, animator->clip);
    if (gui::beginPropertyGrid("animation")) {
        gui::propertyLabel("Pick clip");
        if (ImGui::BeginCombo("##clip", clip ? clip->name.c_str() : "(unknown clip)")) {
            for (const auto& each : clips) {
                const bool current = &each == clip;
                const std::string label = each.name + "  (" + std::to_string(each.duration).substr(0, 4) + " s)";
                if (ImGui::Selectable(label.c_str(), current) && !current) {
                    pushUndo(m_world);
                    animator->clip = each.name;
                    animator->time = std::min(animator->time, each.duration);
                }
            }
            ImGui::EndCombo();
        }
        if (clip) {
            gui::propertyLabel("Scrub");
            if (playing()) {
                ImGui::Text(
                    "%s %.2f / %.2f s", animator->playing ? "Playing" : "Holding",
                    static_cast<double>(m_session->animation()->time(e)), static_cast<double>(clip->duration));
            } else {
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::SliderFloat("##scrub", &animator->time, 0.0f, clip->duration, "%.2f s");
                trackEdit();
                ImGui::SetItemTooltip("The pose the scene starts in; Play starts the clip from here");
            }
        }
        ImGui::EndTable();
    }
    if (!playing()) {
        if (gui::button(m_previewAnimation ? "Stop preview" : "Preview")) m_previewAnimation = !m_previewAnimation;
        ImGui::SetItemTooltip("Play every Animator in the viewport without starting the game");
        gui::toolbarNext(gui::buttonWidth("Show skeleton"));
        if (gui::button(m_showSkeletons ? "Hide skeleton" : "Show skeleton")) m_showSkeletons = !m_showSkeletons;
    }
    size_t skinned = 0, bones = 0;
    for (const auto& mesh : asset->data.meshes)
        if (mesh.skinned()) {
            ++skinned;
            bones = std::max(bones, mesh.bones.size());
        }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(
        "%zu nodes, %zu skinned mesh%s (up to %zu joints each), %zu clips", asset->data.nodes.size(), skinned,
        skinned == 1 ? "" : "es", bones, clips.size());
    ImGui::PopStyleColor();
}

// Lines from each joint to its parent, for the selected animated models.
void Editor::drawSkeletons(const ImVec2& pos, const ImVec2& size) {
    if (!m_showSkeletons) return;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 line = ImGui::GetColorU32(gui::semanticColor(gui::currentTheme().warning));
    for (Entity e : selection()) {
        const auto* renderer = m_world.get<shockwave::ModelRenderer>(e);
        const auto asset = renderer ? engine().assets().model(renderer->model) : nullptr;
        if (!asset || !m_world.has<shockwave::Transform>(e)) continue;
        const auto* pose = m_world.get<shockwave::ModelPose>(e);
        if (pose && pose->model != asset) pose = nullptr;
        // Only nodes that bend a skinned mesh, and the ones between them, are drawn.
        const auto& nodes = asset->data.nodes;
        std::vector<uint8_t> joint(nodes.size(), 0);
        for (const auto& mesh : asset->data.meshes)
            for (const auto& bone : mesh.bones) joint[bone.node] = 1;
        const glm::mat4 world = shockwave::worldMatrix(m_world, e);
        auto place = [&](size_t i) { return glm::vec3(world * (pose ? pose->nodes[i] : nodes[i].world)[3]); };
        for (size_t i = 0; i < nodes.size(); ++i) {
            const int parent = nodes[i].parent;
            if (!joint[i] || parent < 0 || !joint[size_t(parent)]) continue;
            ImVec2 a, b;
            if (worldToScreen(place(i), pos, size, a) && worldToScreen(place(size_t(parent)), pos, size, b)) {
                draw->AddLine(a, b, line, 2.0f);
                draw->AddCircleFilled(a, 3.0f, line);
            }
        }
    }
}

void Editor::drawAnimationStats() {
    const shockwave::AnimationWorld* animation = playing() ? m_session->animation() : m_animation.get();
    if (!animation) return;
    const shockwave::AnimationStats s = animation->stats();
    ImGui::SeparatorText("Animation");
    if (s.animators == 0) {
        ImGui::TextDisabled("No Animators. Drop in a model with animation clips to add one.");
        return;
    }
    if (gui::beginPropertyGrid("animation")) {
        gui::propertyLabel("Animators");
        ImGui::Text("%u (%u cross-fading), %u joints", s.animators, s.blending, s.joints);
        gui::propertyLabel("CPU");
        ImGui::Text("%.3f ms (sampling on all cores)", static_cast<double>(s.updateMs));
        ImGui::EndTable();
    }
    ImGui::TextDisabled("GPU: the \"skinning\" pass under GPU time per pass");
}

}   // namespace editor
