// Audio in the editor: clip previews, placing sources, and spatial audio (docs/SpatialAudio.md):
// Create menu entries, speaker and listener icons with distance rings in the viewport,
// Inspector notes on how a source is heard, Stats and the project's HRTF settings.
#include "Editor.h"

#include <Audio/SpatialAudio.h>
#include <Engine/Hierarchy.h>
#include <Engine/PhysicsComponents.h>
#include <Engine/Systems.h>
#include <UI/Gui.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace editor {

namespace gui = shockwave::gui;
using shockwave::Entity;
using shockwave::kNullEntity;
using shockwave::Transform;

namespace {
ImU32 accentColor(float alpha = 1.0f) {
    ImVec4 c = gui::currentTheme().accent;
    c.w *= alpha;
    return gui::semanticColorU32(c);
}
const char* sideName(const glm::vec3& direction) {
    const float azimuth = glm::degrees(std::atan2(direction.x, -direction.z));
    if (std::abs(azimuth) < 15) return "ahead";
    if (std::abs(azimuth) > 165) return "behind";
    return azimuth > 0 ? "to the right" : "to the left";
}
}   // namespace

void Editor::stopAudioPreview() {
    if (m_audioPreview && m_previewVoice) m_audioPreview->stop(m_previewVoice);
    m_previewVoice = 0;
    m_previewClip.clear();
}
void Editor::previewAudio(const std::string& clip, shockwave::AudioPlayback options) {
    stopAudioPreview();
    options.spatial = false;   // audition without needing a game listener
    if (m_audioPreview) m_previewVoice = m_audioPreview->play(engine().assets().audioClip(clip), options);
    if (m_previewVoice) m_previewClip = clip;
    if (!m_previewVoice) warn("Cannot preview " + clip + (m_audioPreview ? ": " + m_audioPreview->error() : ""));
}
shockwave::Entity Editor::spawnAudio(const std::string& clip, const glm::vec3& position) {
    if (!engine().assets().audioClip(clip)) return shockwave::kNullEntity;
    pushUndo(m_world);
    const auto entity = m_world.create();
    m_world.add<shockwave::Name>(entity, {uniqueName(std::filesystem::path(clip).stem().string())});
    m_world.add<shockwave::Transform>(entity, {position});
    m_world.add<shockwave::AudioSource>(entity, {.clip = clip});
    select(entity);
    log("Placed audio source " + clip);
    return entity;
}

// ---- Create menu --------------------------------------------------------------------------

void Editor::drawAudioCreateItems(Entity parent) {
    ImGui::SeparatorText("Audio");
    auto create = [&](bool listener) {
        if (parent.valid() && !m_world.alive(parent)) parent = kNullEntity;
        pushUndo(m_world);
        const Entity e = m_world.create();
        // A listener starts where the viewport looks from, a source in front of it.
        Transform t = listener ? Transform::fromMatrix(glm::inverse(m_camera.view()))
                               : Transform{placementPoint() + glm::vec3(0.0f, 1.0f, 0.0f)};
        if (parent.valid())
            t = Transform::fromMatrix(glm::inverse(shockwave::worldMatrix(m_world, parent)) * t.matrix());
        m_world.add<shockwave::Name>(e, {uniqueName(listener ? "Audio listener" : "Sound")});
        m_world.add<Transform>(e, t);
        if (listener)
            m_world.add<shockwave::AudioListener>(e);
        else
            m_world.add<shockwave::AudioSource>(e, {.loop = true});
        if (parent.valid()) m_world.add<shockwave::Parent>(e, {parent});
        select(e);
        log("Created " + displayName(e) + (parent.valid() ? " under " + displayName(parent) : ""));
    };
    if (ImGui::MenuItem("Sound source")) create(false);
    ImGui::SetItemTooltip("A looping 3D sound. Pick its clip in the Inspector, or drop a sound from Content");
    if (ImGui::MenuItem("Audio listener")) create(true);
    ImGui::SetItemTooltip("The ears of the scene, looking down -Z. Without one, the primary camera listens");
}

// ---- Viewport -----------------------------------------------------------------------------

// Speakers for sources and a pair of ears for the listener. A selected source shows its
// min (solid) and max (faint) distance as rings, and its facing when it's directional.
void Editor::drawAudioIcons(const ImVec2& pos, const ImVec2& size) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    auto ring = [&](const glm::vec3& center, float radius, ImU32 color) {
        if (!(radius > 0)) return;
        ImVec2 previous;
        bool previousOk = worldToScreen(center + glm::vec3(radius, 0, 0), pos, size, previous);
        for (int i = 1; i <= 48; ++i) {
            const float a = glm::two_pi<float>() * static_cast<float>(i) / 48.0f;
            ImVec2 next;
            const bool ok = worldToScreen(center + glm::vec3(std::cos(a), 0, std::sin(a)) * radius, pos, size, next);
            if (ok && previousOk) draw->AddLine(previous, next, color, 1.2f);
            previous = next;
            previousOk = ok;
        }
    };
    m_world.each<const shockwave::AudioSource, const Transform>(
        [&](Entity e, const shockwave::AudioSource& source, const Transform&) {
            const glm::mat4 world = shockwave::worldMatrix(m_world, e);
            const glm::vec3 origin(world[3]);
            const bool chosen = isSelected(e);
            const ImU32 color =
                chosen ? accentColor() : ImGui::GetColorU32(source.spatial ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            if (chosen && source.spatial) {
                ring(origin, source.minDistance, accentColor());
                ring(origin, source.maxDistance, accentColor(0.4f));
                if (source.directivity > 0) {
                    ImVec2 a, b;
                    const glm::vec3 ahead = glm::normalize(-glm::vec3(world[2]) + glm::vec3(0, 0, 1e-6f));
                    if (worldToScreen(origin, pos, size, a) &&
                        worldToScreen(origin + ahead * std::max(source.minDistance, 1.0f), pos, size, b))
                        draw->AddLine(a, b, accentColor(), 2.0f);
                }
            }
            ImVec2 c;
            if (!worldToScreen(origin, pos, size, c)) return;
            // A speaker: a box and its cone, then a sound wave.
            const float s = 5.0f * dpi;
            draw->AddRectFilled(ImVec2(c.x - s * 1.4f, c.y - s * 0.5f), ImVec2(c.x - s * 0.6f, c.y + s * 0.5f), color);
            draw->AddTriangleFilled(
                ImVec2(c.x - s * 0.6f, c.y - s * 0.5f), ImVec2(c.x + s * 0.4f, c.y - s * 1.2f),
                ImVec2(c.x + s * 0.4f, c.y + s * 1.2f), color);
            draw->AddTriangleFilled(
                ImVec2(c.x - s * 0.6f, c.y - s * 0.5f), ImVec2(c.x + s * 0.4f, c.y + s * 1.2f),
                ImVec2(c.x - s * 0.6f, c.y + s * 0.5f), color);
            draw->PathArcTo(ImVec2(c.x + s * 0.4f, c.y), s * 1.1f, -0.9f, 0.9f, 8);
            draw->PathStroke(color, 0, 1.5f);
        });
    const Entity hearing = shockwave::audioListener(m_world);
    m_world.each<const shockwave::AudioListener, const Transform>([&](Entity e, const shockwave::AudioListener&,
                                                                      const Transform&) {
        ImVec2 c;
        if (!worldToScreen(glm::vec3(shockwave::worldMatrix(m_world, e)[3]), pos, size, c)) return;
        const ImU32 color =
            isSelected(e) ? accentColor() : ImGui::GetColorU32(e == hearing ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        // Headphones: a band over two ear cups.
        const float s = 6.0f * dpi;
        draw->PathArcTo(c, s, glm::pi<float>(), glm::two_pi<float>(), 12);
        draw->PathStroke(color, 0, 1.5f);
        draw->AddRectFilled(
            ImVec2(c.x - s * 1.3f, c.y - s * 0.1f), ImVec2(c.x - s * 0.6f, c.y + s * 0.9f), color, 2.0f);
        draw->AddRectFilled(
            ImVec2(c.x + s * 0.6f, c.y - s * 0.1f), ImVec2(c.x + s * 1.3f, c.y + s * 0.9f), color, 2.0f);
    });
}

// ---- Inspector ----------------------------------------------------------------------------

void Editor::drawSpatialAudioNotes(const shockwave::ComponentInfo& info, Entity e) {
    const ImVec4 warning = gui::semanticColor(gui::currentTheme().warning);
    if (info.id == shockwave::componentId<shockwave::AudioListener>()) {
        const Entity hearing = shockwave::audioListener(m_world);
        if (!m_world.has<Transform>(e))
            ImGui::TextColored(warning, "Add a Transform: a listener needs a place to hear from");
        else if (hearing == e)
            ImGui::TextDisabled("Hears the scene's spatial sound, looking down -Z");
        else
            ImGui::TextColored(
                warning, "Not used: '%s' is the listener (the first one wins)", displayName(hearing).c_str());
        return;
    }
    if (info.id != shockwave::componentId<shockwave::AudioSource>()) return;
    const auto& source = *m_world.get<shockwave::AudioSource>(e);
    if (!source.spatial) return;
    if (!shockwave::audioListener(m_world).valid())
        ImGui::TextColored(warning, "Silent: no Audio listener or primary camera to hear it");
    if (source.occlusion && !m_world.has<shockwave::Collider>(e) && m_world.count<shockwave::Collider>() == 0)
        ImGui::TextColored(warning, "Occlusion needs colliders to block it; this scene has none");
    ImGui::TextDisabled("Rendered with %s", shockwave::spatialBackendName().c_str());
    ImGui::SetItemTooltip("%s", shockwave::steamAudioStatus().c_str());
    // During Play: how the listener hears it right now.
    const shockwave::AudioWorld* audio = m_session ? m_session->audio() : nullptr;
    if (!audio) return;
    for (const auto& report : audio->report()) {
        if (!(report.entity == e)) continue;
        const auto& h = report.heard;
        ImGui::Text(
            "Heard %.1f m away %s, volume %.0f%%%s", static_cast<double>(h.distance), sideName(h.direction),
            static_cast<double>(h.distanceGain * h.directivity * 100.0f), report.playing ? "" : " (stopped)");
        if (source.occlusion)
            ImGui::Text("%.0f%% of occlusion rays get through", static_cast<double>(h.occlusion * 100.0f));
    }
}

// ---- Stats --------------------------------------------------------------------------------

void Editor::drawAudioStats() {
    ImGui::SeparatorText("Audio");
    shockwave::AudioWorld* session = m_session ? m_session->audio() : nullptr;
    shockwave::AudioMixer* mixer = session ? &session->mixer() : m_audioPreview.get();
    if (gui::beginPropertyGrid("audio_stats")) {
        gui::propertyLabel("Spatial");
        ImGui::TextUnformatted(shockwave::spatialBackendName().c_str());
        ImGui::SetItemTooltip("%s", shockwave::steamAudioStatus().c_str());
        if (mixer) {
            const shockwave::AudioMixerStats s = mixer->stats();
            gui::propertyLabel(session ? "Voices (Play)" : "Voices (preview)");
            ImGui::Text("%d, %d of %d through Steam Audio", s.voices, s.spatialVoices, s.spatialSlots);
            gui::propertyLabel("Mix cost");
            ImGui::Text(
                "%.1f us per 5.3 ms block (peak %.1f)", static_cast<double>(s.mixMicroseconds),
                static_cast<double>(s.peakMicroseconds));
            ImGui::SetItemTooltip("Runs on the audio thread, not the frame. Under 5333 us keeps up in real time");
        }
        ImGui::EndTable();
    }
}

// ---- Project settings ---------------------------------------------------------------------

// Rows inside the Project settings grid. The change applies to Play in the editor at
// once, and is saved with the project.
void Editor::drawAudioProjectSettings() {
    gui::propertyLabel("Spatial audio");
    int mode = m_projectEdit.audioHrtf ? 0 : 1;
    static const char* modes[] = {"Headphones (HRTF)", "Speakers (panning)"};
    if (ImGui::Combo("##audio_hrtf", &mode, modes, IM_ARRAYSIZE(modes))) m_projectEdit.audioHrtf = mode == 0;
    ImGui::SetItemTooltip("How 3D sound reaches the ears; player --audio-hrtf overrides it (audio_hrtf)");
    gui::propertyLabel("HRTF quality");
    int quality = m_projectEdit.audioHrtfHigh ? 1 : 0;
    static const char* qualities[] = {"Low", "High"};
    ImGui::BeginDisabled(!m_projectEdit.audioHrtf);
    if (ImGui::Combo("##audio_hrtf_quality", &quality, qualities, IM_ARRAYSIZE(qualities)))
        m_projectEdit.audioHrtfHigh = quality == 1;
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("High blends between measured directions; low snaps to the nearest (audio_hrtf_quality)");
    applyAudioSettings(m_projectEdit);
}

void Editor::applyAudioSettings(const shockwave::Project& project) {
    shockwave::setAudioSettings({.hrtf = project.audioHrtf, .hrtfBilinear = project.audioHrtfHigh});
}

}   // namespace editor
