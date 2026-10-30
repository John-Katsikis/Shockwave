// Window > Lighting (G05, G06; docs/Lighting.md): the scene's bake, its state and why it's
// out of date, and Bake. The bake runs on a copy of the scene on a thread of its own, with
// the same bakeLighting() as `player --bake-lighting`; the viewport picks the result up when
// it's published, and the previous bake stays until then (or if it's cancelled).
#include "Editor.h"

#include <Engine/BakedLighting.h>
#include <Engine/SceneFile.h>
#include <Lighting/LightingBaker.h>
#include <UI/Gui.h>

#include <atomic>
#include <sstream>
#include <thread>

namespace editor {

namespace gui = shockwave::gui;

struct LightingBakeJob {
    shockwave::World world;   // the scene as it was when Bake was pressed
    shockwave::LightingBakeSettings settings;
    shockwave::LightingBakeProgress progress;
    shockwave::LightingBakeReport report;
    std::string error;
    bool ok = false;
    std::atomic<bool> done{false};
    std::thread thread;
    ~LightingBakeJob() {
        progress.cancel = true;
        if (thread.joinable()) thread.join();
    }
};

void Editor::startLightingBake() {
    if (m_lightingBake || !m_hasProject || m_scenePath.empty() || playing()) return;
    // A copy through the scene format: exactly what saving would keep, and nothing the
    // editor changes while the bake runs.
    auto job = std::make_shared<LightingBakeJob>();
    std::stringstream text;
    shockwave::writeScene(text, m_world, engine().assets());
    std::string error;
    if (!shockwave::readScene(job->world, engine().assets(), text, m_scenePath.string(), error)) {
        m_lightingResult = "Couldn't copy the scene to bake: " + error;
        return;
    }
    job->settings.quality = m_lightingQuality ? shockwave::LightingBakeSettings::Quality::Final
                                              : shockwave::LightingBakeSettings::Quality::Preview;
    job->settings.seed = uint32_t(std::max(1, m_lightingSeed));
    const std::filesystem::path content = engine().assets().contentRoot();
    const std::string scene = m_project.relative(m_scenePath);
    shockwave::Assets& assets = engine().assets();
    LightingBakeJob* raw = job.get();
    job->thread = std::thread([raw, &assets, content, scene] {
        raw->ok = shockwave::bakeLighting(
            raw->world, assets, content, scene, raw->settings, raw->report, raw->error, &raw->progress);
        raw->done = true;
    });
    m_lightingBake = std::move(job);
    m_lightingResult.clear();
    log("Baking lighting for " + scene + " (" + shockwave::lightingQualityName(m_lightingBake->settings.quality) + ")");
}

void Editor::finishLightingBake() {
    LightingBakeJob& job = *m_lightingBake;
    job.thread.join();
    if (!job.ok) {
        m_lightingResult = "Bake failed: " + job.error;
        warn(m_lightingResult);
    } else {
        m_lightingResult = job.report.text();
        log("Baked lighting: " + job.report.manifest);
        // The scene draws its bake through BakedLighting: the first bake adds it (one undo step).
        shockwave::BakedLighting* bake = nullptr;
        m_world.each<shockwave::BakedLighting>([&](shockwave::Entity, shockwave::BakedLighting& b) {
            if (!bake) bake = &b;
        });
        if (!bake || bake->asset != job.report.manifest) {
            pushUndo(m_world);
            if (!bake) {
                shockwave::Entity holder = shockwave::kNullEntity;
                m_world.each<const shockwave::Environment>(
                    [&](shockwave::Entity e, const shockwave::Environment&) { holder = e; });
                if (!holder.valid()) {
                    holder = m_world.create();
                    m_world.add<shockwave::Name>(holder, {uniqueName("Baked lighting")});
                }
                bake = &m_world.add<shockwave::BakedLighting>(holder);
            }
            bake->asset = job.report.manifest;
            log("The scene now draws " + job.report.manifest + " (BakedLighting)");
        }
        m_bakedLighting.reload();
    }
    m_lightingBake.reset();
}

void Editor::cancelLightingBake() {
    m_lightingBake.reset();   // cancels and waits; the published bake is untouched
}

void Editor::drawLightingWindow() {
    if (m_lightingBake && m_lightingBake->done) finishLightingBake();
    if (!m_showLighting) return;
    ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Lighting", &m_showLighting)) {
        ImGui::End();
        return;
    }
    if (!m_hasProject) {
        ImGui::TextDisabled("Open a project to bake its lighting.");
        ImGui::End();
        return;
    }
    const ImVec4 errorColor = gui::semanticColor(gui::currentTheme().error);
    const ImVec4 warningColor = gui::semanticColor(gui::currentTheme().warning);
    using State = shockwave::BakedLightingRuntime::State;
    const auto& status = m_bakedLighting.status();

    ImGui::SeparatorText("This scene's bake");
    if (status.state == State::None) {
        ImGui::TextWrapped("Not baked. Mark walls and floors with BakeGeometry (mobility static, receive_lightmap) "
                           "and add a DiffuseProbeVolume where things move, then Bake.");
    } else {
        ImGui::Text("%s", status.asset.c_str());
        const ImVec4 color =
            status.state == State::Ready ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
            : status.state == State::Stale || status.state == State::Disabled || status.state == State::Missing
                ? warningColor
                : errorColor;
        ImGui::TextColored(
            color, "%s%s%s", shockwave::BakedLightingRuntime::stateName(status.state),
            status.reason.empty() ? "" : ": ", status.reason.c_str());
        ImGui::Text(
            "%u of %u receivers drawn with their lightmap, %u probe volumes (%u probes), %.1f MB on the GPU",
            status.boundReceivers, status.receivers, status.volumes, status.probes,
            double(status.gpuBytes) / (1024.0 * 1024.0));
    }
    if (!engine().renderer().bakedLighting())
        ImGui::TextColored(warningColor, "Baked lighting is off in the graphics settings: the viewport ignores it.");

    // Ids: every participant needs one before it can bake (never fixed silently).
    const std::vector<std::string> idProblems = shockwave::checkLightingIds(m_world);
    if (!idProblems.empty()) {
        ImGui::SeparatorText("Ids");
        for (const std::string& p : idProblems) ImGui::TextColored(errorColor, "%s", p.c_str());
        ImGui::BeginDisabled(playing());
        if (gui::button("Assign missing ids")) {
            pushUndo(m_world);
            log("Assigned " + std::to_string(shockwave::assignLightingIds(m_world)) + " bake ids");
        }
        ImGui::EndDisabled();
    }

    ImGui::SeparatorText("Bake");
    const bool running = m_lightingBake != nullptr;
    ImGui::BeginDisabled(running);
    static const char* qualities[] = {"Preview (64 paths a texel)", "Final (512 paths a texel)"};
    ImGui::SetNextItemWidth(240.0f * ImGui::GetFontSize() / 13.0f);
    ImGui::Combo("Quality", &m_lightingQuality, qualities, IM_ARRAYSIZE(qualities));
    ImGui::SetNextItemWidth(120.0f * ImGui::GetFontSize() / 13.0f);
    ImGui::InputInt("Seed", &m_lightingSeed);
    ImGui::SetItemTooltip("The same scene, settings and seed always bake the same files");
    ImGui::EndDisabled();
    const char* why = playing()             ? "Stop playing to bake"
                      : m_scenePath.empty() ? "Save the scene first: the bake is named after it"
                      : !idProblems.empty() ? "Assign the missing ids first"
                                            : nullptr;
    if (!running) {
        ImGui::BeginDisabled(why != nullptr);
        if (gui::button("Bake")) startLightingBake();
        ImGui::EndDisabled();
        if (why) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", why);
        }
    } else {
        const std::string stage = m_lightingBake->progress.stage();
        ImGui::ProgressBar(m_lightingBake->progress.fraction.load(), ImVec2(-1.0f, 0.0f), stage.c_str());
        if (gui::button("Cancel")) {
            cancelLightingBake();
            m_lightingResult = "Cancelled; the last bake is still in place";
        }
    }
    if (!m_lightingResult.empty()) {
        ImGui::SeparatorText("Last bake");
        ImGui::TextWrapped("%s", m_lightingResult.c_str());
    }
    ImGui::End();
}

}   // namespace editor
