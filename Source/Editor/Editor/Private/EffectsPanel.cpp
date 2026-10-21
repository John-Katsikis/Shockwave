// Particle effects in the editor: ParticleEffects play live in the viewport while editing
// (so tuning one shows straight away), restart when Play starts and stops, and freeze
// while paused. The Inspector picks the effect file and shows its problems and particle
// count, the Stats window what effects cost. See docs/Effects.md.
#include "Editor.h"

#include <Effects/Effects.h>
#include <UI/Gui.h>

#include <SDL3/SDL.h>

#include <filesystem>

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;
using shockwave::Entity;

void Editor::updateEffects(double dt) {
    if (!m_effects || !m_hasProject) return;
    m_effects->setEnabled(m_showEffects);
    m_effects->update(m_world, playState() == PlayState::Paused ? 0.0 : dt);
}

void Editor::restartEffects() {
    if (m_effects) m_effects->restart();
}

Entity Editor::createParticleEffect(const std::string& effect) {
    pushUndo(m_world);
    const Entity e = m_world.create();
    std::string name = effect.empty() ? std::string("Particle effect") : fs::path(effect).stem().string();
    if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    m_world.add<shockwave::Name>(e, {uniqueName(name)});
    m_world.add<shockwave::Transform>(e, {placementPoint() + glm::vec3(0.0f, 1.0f, 0.0f)});
    shockwave::ParticleEffect component;
    component.effect = effect;
    m_world.add<shockwave::ParticleEffect>(e, component);
    select(e);
    if (effect.empty())
        log("Created " + displayName(e) +
            ". Pick its effect in the Inspector; make effects with the free Effekseer editor (effekseer.github.io)");
    else
        log("Created " + displayName(e) + " playing " + effect);
    return e;
}

void Editor::drawParticleEffectNotes(Entity e) {
    auto* component = m_world.get<shockwave::ParticleEffect>(e);
    if (!component || !m_effects) return;

    // The effect file: any .efkefc in Content.
    const std::vector<std::string> files = m_effects->effectFiles();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(
            "##effect", component->effect.empty() ? "Pick an effect..." : component->effect.c_str(),
            ImGuiComboFlags_HeightLarge)) {
        if (files.empty()) ImGui::TextDisabled("No .efkefc files in Content yet");
        for (const std::string& file : files) {
            if (ImGui::Selectable(file.c_str(), file == component->effect) && file != component->effect) {
                pushUndo(m_world);
                component->effect = file;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("An .efkefc file in Content, saved by the Effekseer editor");

    if (!component->effect.empty()) {
        for (const auto& problem : m_effects->validate(component->effect)) {
            const ImVec4 color = problem.warning ? gui::currentTheme().warning : gui::currentTheme().error;
            ImGui::PushStyleColor(ImGuiCol_Text, gui::semanticColor(color));
            ImGui::TextWrapped("%s", problem.message.c_str());
            ImGui::PopStyleColor();
        }
    }

    if (gui::button("Restart")) m_effects->restart(e);
    ImGui::SetItemTooltip("Play it again from its first frame");
    gui::toolbarNext(gui::buttonWidth("Reload"));
    if (gui::button("Reload")) {
        m_effects->reloadAll();
        log("Reloaded every particle effect");
    }
    ImGui::SetItemTooltip("Effects reload by themselves when their files or textures change");
    const fs::path file = m_project.assetFolder / component->effect;
    std::error_code ec;
    const bool exists = !component->effect.empty() && fs::is_regular_file(file, ec);
    gui::toolbarNext(gui::buttonWidth("Show file"));
    ImGui::BeginDisabled(!exists);
    if (gui::button("Show file")) SDL_OpenURL(("file://" + fs::absolute(file.parent_path()).generic_string()).c_str());
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Open its folder, to edit it in Effekseer");

    if (!m_showEffects) {
        ImGui::TextDisabled("Particle effects are hidden (viewport View menu)");
    } else if (exists) {
        const float duration = m_effects->duration(component->effect);
        const uint32_t particles = m_effects->particleCount(e);
        if (duration < 0.0f)
            ImGui::TextDisabled("%u particles live; runs until stopped", particles);
        else
            ImGui::TextDisabled("%u particles live; one run lasts %.1f s", particles, static_cast<double>(duration));
    }
}

void Editor::drawEffectsStats() {
    if (!m_effects) return;
    const shockwave::EffectsStats s = m_effects->stats();
    ImGui::SeparatorText("Particle effects");
    if (s.playing == 0) {
        ImGui::TextDisabled("None playing. Create > Particle effect adds one.");
        return;
    }
    if (gui::beginPropertyGrid("effects")) {
        gui::propertyLabel("Playing");
        ImGui::Text("%u effects, %u particles", s.playing, s.particles);
        gui::propertyLabel("Draw list");
        ImGui::Text("%u draws, %u vertices", s.batches, s.vertices);
        if (s.skippedBatches) {
            gui::propertyLabel("Skipped");
            ImGui::Text("%u draws (distortion, custom materials)", s.skippedBatches);
        }
        gui::propertyLabel("CPU");
        ImGui::Text(
            "%.3f ms simulation (worker), %.3f ms build", static_cast<double>(s.updateMs),
            static_cast<double>(s.buildMs));
        gui::propertyLabel("Waited");
        ImGui::Text("%.3f ms for the simulation, on the main thread", static_cast<double>(s.waitMs));
        const shockwave::ParticleStats upload = engine().renderer().particleStats();
        gui::propertyLabel("Upload");
        ImGui::Text(
            "%.3f ms, %.2f MB to the GPU", static_cast<double>(upload.uploadMs),
            static_cast<double>(upload.uploadBytes) / 1e6);
        ImGui::EndTable();
    }
    ImGui::TextDisabled("GPU: the \"particles\" pass under GPU time per pass");
}

}   // namespace editor
