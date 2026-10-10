// Playing a project: load the startup scene, run its game module and physics, draw it,
// and take the screenshot asked for.
#include "Player.h"

#include <Assets/AssetFolder.h>
#include <Assets/BuiltinAssets.h>
#include <Core/Log.h>
#include <Engine/Engine.h>
#include <Engine/SceneFile.h>
#include <Engine/Systems.h>
#include <GameFramework/GameModule.h>
#include <GameFramework/PlaySession.h>
#include <Input/InputBinding.h>
#include <Physics/Physics.h>
#include <Projects/Project.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace player {

void Player::onStart() {
    const auto& module = shockwave::projectGameModule();
    if (module.registerTypes) module.registerTypes();
    shockwave::createBuiltinAssets(engine().renderer(), engine().assets());
    if (module.createAssets) module.createAssets(engine());
    m_instance = shockwave::makeGameInstance(&module);
    m_instance->init(&engine());
    m_assets = std::make_unique<shockwave::AssetFolder>(engine().renderer(), engine().assets(), engine().jobs());
    std::vector<std::string> importProblems;
    for (const auto& change : m_assets->open(m_project.assetFolder)) {
        if (change.what == shockwave::AssetFolder::Change::What::Failed)
            importProblems.push_back(
                m_project.relative(m_project.assetFolder / change.name) + ": import failed: " + change.error);
    }
    if (m_tool.mode != Tool::Mode::Play) {
        m_exitCode = m_tool.mode == Tool::Mode::Check ? check(importProblems) : schema();
        engine().quit();
        return;
    }
    if (!importProblems.empty()) throw std::runtime_error("Asset " + importProblems.front());
    if (!m_tool.scene.empty()) m_project.startupScene = m_tool.scene;
    engine().renderer().setDebugView(m_tool.view);
    std::string error;
    std::vector<std::string> warnings;
    shockwave::SceneFileHooks hooks;
    // Scenes without a Camera entity fall back to the view an older editor saved.
    hooks.readHeader = [&](const std::string& key, std::istream& in) {
        if (key == "editor_camera" || key == "camera")
            in >> m_camera.position.x >> m_camera.position.y >> m_camera.position.z >> m_camera.yaw >> m_camera.pitch;
    };
    if (!shockwave::loadScene(m_world, engine().assets(), m_project.startupScene.string(), error, hooks, &warnings))
        throw std::runtime_error(error);
    if (!warnings.empty()) throw std::runtime_error("Scene has unresolved content: " + warnings.front());
    shockwave::FlyCamera::bindDefaultControls(engine().input());
    if (const auto bindings = shockwave::inputBindingsPath(m_project.root); std::filesystem::exists(bindings)) {
        if (!engine().input().loadBindingsFile(bindings, error)) throw std::runtime_error(error);
    }
    m_session = std::make_unique<shockwave::PlaySession>(m_world, engine().jobs(), *m_instance, &engine());
    m_session->start();
    if (!m_session->physics()->diagnostics().empty()) {
        const auto& problem = m_session->physics()->diagnostics().front();
        throw std::runtime_error(
            "Physics setup failed for entity " + std::to_string(problem.entity.index) + ": " + problem.message);
    }
    SW_LOG(
        shockwave::LogPhysics, Info, "{} bodies, {} characters", m_session->physics()->bodyCount(),
        m_session->physics()->characterCount());
    SW_LOG(
        shockwave::LogEngine, Info, "project {}; module {}; startup scene {}", m_project.name, m_project.gameModule,
        m_project.relative(m_project.startupScene));
}

void Player::onUpdate(double dt) {
    if (m_session) m_session->update(dt);
}

void Player::onFrame(double dt) {
    if (!m_session) return;
    m_session->frame(dt);
    if (!shockwave::primaryCamera(m_world).valid()) m_camera.update(engine().input(), dt);
    if (!m_tool.screenshot.empty()) takeScreenshot();
}

void Player::onRender(shockwave::Renderer& renderer, double alpha) {
    // The game looks through its primary Camera, wherever gameplay has moved it.
    glm::mat4 view, projection;
    if (shockwave::cameraMatrices(
            m_world, shockwave::primaryCamera(m_world), renderer.aspectRatio(), view, projection)) {
        renderer.setCamera(view, projection);
    } else {
        renderer.setCamera(m_camera.view(), m_camera.projection(renderer.aspectRatio()));
    }
    shockwave::renderWorld(m_world, renderer, engine().jobs(), alpha, &engine().assets());
}

void Player::onShutdown() {
    if (m_tool.printFrame) std::printf("Render graph (last frame):\n%s", engine().renderer().frameGraph().c_str());
    if (m_session) {
        SW_LOG(shockwave::LogGame, Info, "simulation ticks: {}", m_session->ticks());
        m_session->stop();
    }
    if (m_instance) m_instance->shutdown();
}

void Player::takeScreenshot() {
    auto& renderer = engine().renderer();
    ++m_frame;
    if (!m_screenshotRequested) {
        if (m_frame >= m_tool.screenshotFrame) {
            renderer.requestScreenshot(m_tool.screenshot.string());
            m_screenshotRequested = true;
        }
    } else if (!renderer.screenshotPending()) {
        if (renderer.screenshotResult().empty()) {
            std::printf(
                "Wrote %s (%ux%u, %u lights)\n", m_tool.screenshot.string().c_str(), renderer.renderWidth(),
                renderer.renderHeight(), renderer.lightCount());
        } else {
            std::fprintf(stderr, "Player: %s\n", renderer.screenshotResult().c_str());
            m_exitCode = 1;
        }
        engine().quit();
    }
}

}   // namespace player
