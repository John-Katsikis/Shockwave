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
    if (!m_session || playState() == PlayState::Playing) return;
    if (!m_gameModuleMatches) {
        warn("Play is off: this project's gameplay module isn't the one compiled into the editor");
        return;
    }
    const bool resuming = playState() == PlayState::Paused;
    try {
        m_session->start();
    } catch (const std::exception& e) {
        error(std::string("Couldn't start gameplay: ") + e.what());
        return;
    }
    if (resuming) {
        log("Resumed");
        return;
    }
    if (const auto* physics = m_session->physics()) {
        for (const auto& problem : physics->diagnostics()) warn(displayName(problem.entity) + ": " + problem.message);
    }
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
    if (!m_session) return;
    if (playState() == PlayState::Playing) {
        m_session->pause();
        log("Paused");
    } else if (playState() == PlayState::Paused) {
        play();
    }
}

void Editor::stop() {
    if (!m_session || !playing()) return;
    try {
        m_session->stop();
    } catch (const std::exception& e) {
        error(std::string("Gameplay cleanup failed: ") + e.what());
    }
    m_gizmoWasUsing = false;
    m_viewThroughCamera = m_viewBeforePlay;
    log("Stopped after " + std::to_string(m_session->ticks()) + " simulation ticks");
}

}   // namespace editor
