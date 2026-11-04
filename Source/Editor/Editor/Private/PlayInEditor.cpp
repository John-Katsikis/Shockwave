// Play in the editor: Play, Pause and Stop on the open scene through a PlaySession.
#include "Editor.h"

#include <Assets/BuiltinAssets.h>
#include <Physics/Physics.h>
#include <UI/Gui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <Engine/SceneFile.h>
#include <sstream>

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;
using shockwave::Entity;
using shockwave::kNullEntity;
using shockwave::MeshRenderer;
using shockwave::Name;
using shockwave::Parent;
using shockwave::Transform;

// ---- Play mode ----------------------------------------------------------------------------

void Editor::play() {
    stopAudioPreview();
    if (!m_runtimeLevels || playState() == PlayState::Playing) return;
    if (!m_gameModuleMatches) {
        warn(
            "Play is off: this project's game module \"" + m_project.gameModule +
            "\" isn't built. Build > Build and reload game code builds it.");
        return;
    }
    const bool resuming = playState() == PlayState::Paused;
    updateNavigation(0.0);   // loads the scene's saved navigation mesh, so Play needn't bake
    if (!resuming) restartEffects();   // effects play from their first frame, as in the game
    if (!resuming) restartAnimation();
    try {
        if (!resuming) {   // game.saves() works in Saved/SaveGames
            shockwave::GraphicsSettings defaults;
            std::string problem;
            if (!shockwave::projectGraphicsSettings(m_project, defaults, problem) ||
                !engine().graphics().configure(defaults, {}, problem)) {
                error("Couldn't load game graphics defaults: " + problem);
                return;
            }
            // The authored scene waits aside; Play runs in a copy (docs/LevelLifetime.md).
            std::vector<shockwave::Diagnostic> ignored;
            if (!shockwave::projectLevels(m_project, m_playCatalog, ignored))
                m_playCatalog = shockwave::implicitLevelCatalog(m_project);
            std::string level = shockwave::kImplicitLevelId;
            for (const shockwave::LevelEntry& entry : m_playCatalog.levels)
                if (!m_scenePath.empty() &&
                    (m_project.root / entry.scene).lexically_normal() == m_scenePath.lexically_normal())
                    level = entry.id;
            // Level leases (docs/AssetResidency.md), unless the Resource residency window
            // turned them off.
            m_residency.setBudget({uint64_t(double(m_budgetCpuMiB) * 1024 * 1024),
                                   uint64_t(double(m_budgetGpuMiB) * 1024 * 1024)});
            m_runtimeLevels->setResidency(m_residencyEnabled ? &m_residency : nullptr);
            m_selectionBeforePlay = m_selection;
            m_authoredDuringPlay.emplace(std::move(m_world));
            m_world = *m_authoredDuringPlay;
            try {
                m_runtimeLevels->start({level});
            } catch (...) {
                m_world = std::move(*m_authoredDuringPlay);
                m_authoredDuringPlay.reset();
                throw;
            }
        } else {
            session()->start();
        }
    } catch (const std::exception& e) {
        error(std::string("Couldn't start gameplay: ") + e.what());
        return;
    }
    if (resuming) {
        log("Resumed");
        return;
    }
    if (const auto* physics = session()->physics()) {
        for (const auto& problem : physics->diagnostics()) warn(displayName(problem.entity) + ": " + problem.message);
    }
    reportNavigationProblems();
    m_viewBeforePlay = m_viewThroughCamera;
    if (m_prefs.playFromSceneCamera) {
        if (shockwave::primaryCamera(m_world).valid()) {
            m_viewThroughCamera = true;
        } else {
            warn("The scene has no Camera, so Play shows the editor's view. Add one with Create > Camera.");
        }
    }
    log("Playing the project's gameplay and physics. Stop restores the scene; F8 switches cameras.");
}

void Editor::pause() {
    if (!session()) return;
    if (playState() == PlayState::Playing) {
        if (networkPlay()) {
            warn("Pause is off during network play: the host never pauses for one player");
            return;
        }
        if (session()->pause()) log("Paused");
    } else if (playState() == PlayState::Paused) {
        play();
    }
}

void Editor::stop() {
    stopAudioPreview();
    if (!m_runtimeLevels || (!playing() && !m_authoredDuringPlay)) return;   // a failed travel left nothing playing
    const bool wasOnline = networkPlay();
    const uint64_t ticks = session() ? session()->ticks() : 0;
    try {
        m_runtimeLevels->stop();
    } catch (const std::exception& e) {
        error(std::string("Gameplay cleanup failed: ") + e.what());
    }
    // The authored scene, exactly as it was, with its selection; undo never saw Play.
    if (m_authoredDuringPlay) m_world = std::move(*m_authoredDuringPlay);
    m_authoredDuringPlay.reset();
    m_selection = std::move(m_selectionBeforePlay);
    m_selectionBeforePlay.clear();
    restartEffects();
    restartAnimation();
    m_gizmoWasUsing = false;
    m_viewThroughCamera = m_viewBeforePlay;
    log("Stopped after " + std::to_string(ticks) + " simulation ticks");
    // What network play held back: game code, then content (the asset folder catches up on its next poll).
    if (wasOnline && std::exchange(m_reloadAfterNetworkPlay, false)) m_reloadGameCodeRequested = true;
}

shockwave::LevelHost::Hooks Editor::levelHooks() {
    shockwave::LevelHost::Hooks hooks;
    hooks.load = [this](const shockwave::LevelRequest& request, shockwave::World& into, std::string& problem) {
        const shockwave::LevelEntry* entry = m_playCatalog.find(request.level);
        if (!entry) {
            problem = "no level \"" + request.level + "\" in the level catalog";
            return false;
        }
        const fs::path scene = (m_project.root / entry->scene).lexically_normal();
        std::vector<std::string> warnings;
        if (!shockwave::loadScene(into, engine().assets(), scene.string(), problem, {}, &warnings)) return false;
        if (!warnings.empty()) {
            problem = entry->scene + " has unresolved content: " + warnings.front();
            return false;
        }
        std::string navigation;
        (void)shockwave::loadSceneNavMesh(scene, navigation);   // or Play bakes it
        return true;
    };
    hooks.configure = [this](shockwave::PlaySession& session, const shockwave::LevelRequest& request) {
        const shockwave::LevelEntry* entry = m_playCatalog.find(request.level);
        const std::string scene = entry ? entry->scene : (m_scenePath.empty() ? "" : m_project.relative(m_scenePath));
        session.setSaveFolder(saveGamesFolder(), scene);
        session.setGameCircuit(m_project.gameCircuitPath());
        if (m_playNetworkStart) session.setNetworkStart(*m_playNetworkStart, m_playServices);
    };
    // What a level needs resident: its scene as saved, and everything that reaches.
    hooks.assets = [this](const shockwave::LevelRequest& request, const shockwave::World&,
                          std::vector<shockwave::AssetCost>& assets, std::string&) {
        const shockwave::LevelEntry* entry = m_playCatalog.find(request.level);
        const fs::path scene = entry ? (m_project.root / entry->scene).lexically_normal() : m_scenePath;
        if (scene.empty()) return true;   // never saved: nothing on disk to follow
        shockwave::AssetGraph& graph = assetGraph();
        const std::string id = graph.idFor(fs::absolute(scene));
        if (!id.empty()) assets = shockwave::assetCosts(graph, id, m_assetFolder.get(), &engine().assets());
        return true;
    };
    hooks.started = [this](shockwave::PlaySession&, const shockwave::LevelRequest& request) {
        if (!m_authoredDuringPlay || m_runtimeLevels->epoch() <= 1) return;
        // Another level: nothing selected in the authored scene is in it.
        m_selection.clear();
        m_gizmoWasUsing = false;
        restartEffects();
        restartAnimation();
        log("Level \"" + request.level + "\"");
    };
    return hooks;
}

void Editor::processTravel() {
    if (!m_runtimeLevels || !playing()) return;
    m_runtimeLevels->process();
    if (!m_runtimeLevels->playing() && !m_runtimeLevels->failure().empty()) {
        error(m_runtimeLevels->failure() + "; Play stopped");
        stop();
    }
}

// ---- --level-cycles --------------------------------------------------------------------

void Editor::runLevelCycles() {
    LevelCycles& c = m_cycles;
    auto sceneText = [&] {
        std::ostringstream out;
        shockwave::writeScene(out, m_world, engine().assets());
        return out.str();
    };
    auto fail = [&](const std::string& why) {
        if (c.problems.size() < 20) c.problems.push_back(why);
    };
    if (c.step < 0) {
        // One edit to undo and a selection, so there's history and a selection to keep.
        pushUndo(m_world);
        if (m_world.entityCount() > 0) select(m_world.entityAt(0));
        c.scene = sceneText();
        c.undo = m_undo.size();
        c.redo = m_redo.size();
        c.selection = m_selection;
        c.cameraPosition = m_camera.position;
        c.cameraYaw = m_camera.yaw;
        c.cameraPitch = m_camera.pitch;
        c.step = 0;
        return;
    }
    const int cycle = c.step / 3, phase = c.step % 3;
    if (cycle >= m_options.levelCycles) {
        if (sceneText() != c.scene) fail("the authored scene changed");
        if (m_undo.size() != c.undo || m_redo.size() != c.redo) fail("the undo history changed");
        if (m_selection != c.selection) fail("the selection changed");
        if (m_camera.position != c.cameraPosition || m_camera.yaw != c.cameraYaw || m_camera.pitch != c.cameraPitch)
            fail("the editor camera moved");
        for (const auto& p : c.problems) std::fprintf(stderr, "level cycles: %s\n", p.c_str());
        std::printf(
            "level cycles: %d Play/travel/Stop cycles, %s\n", m_options.levelCycles,
            c.problems.empty() ? "the authored scene, undo, selection and camera are unchanged" : "FAILED");
        m_exitCode = c.problems.empty() ? 0 : 1;
        m_options.levelCycles = 0;
        engine().quit();
        return;
    }
    ++c.step;
    switch (phase) {
    case 0:
        play();
        if (!playing()) {
            fail("Play didn't start (cycle " + std::to_string(cycle) + ")");
            c.step = m_options.levelCycles * 3;
        }
        break;
    case 1: {   // travel: the catalog's next level, or any other one
        const std::string from = m_runtimeLevels->current().level;
        const shockwave::LevelEntry* here = m_playCatalog.find(from);
        c.to = here && !here->next.empty() ? here->next : std::string();
        for (const auto& level : m_playCatalog.levels)
            if (c.to.empty() && level.id != from) c.to = level.id;
        if (c.to.empty() || !m_runtimeLevels->request({c.to})) fail("couldn't ask for another level from " + from);
        break;   // processTravel() carries it out at the end of this frame
    }
    case 2:
        if (!playing() || m_runtimeLevels->current().level != c.to)
            fail("cycle " + std::to_string(cycle) + " didn't arrive in \"" + c.to + "\"");
        if (!m_selection.empty()) fail("the selection survived travel into another level");
        stop();
        if (sceneText() != c.scene) fail("cycle " + std::to_string(cycle) + ": Stop didn't restore the authored scene");
        if (m_selection != c.selection) fail("cycle " + std::to_string(cycle) + ": Stop didn't restore the selection");
        break;
    }
}

}   // namespace editor
