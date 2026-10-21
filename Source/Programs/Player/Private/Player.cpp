// Playing a project: load the startup scene, run its game module and physics, draw it,
// and take the screenshot asked for.
#include "Player.h"

#include <Animation/AnimationWorld.h>
#include <Assets/AssetFolder.h>
#include <Assets/BuiltinAssets.h>
#include <Audio/Audio.h>
#include <Circuits/Circuit.h>
#include <Core/Log.h>
#include <Engine/Engine.h>
#include <Engine/Prefab.h>
#include <Engine/SceneFile.h>
#include <Engine/Systems.h>
#include <GameFramework/GameModule.h>
#include <GameFramework/NetworkPump.h>
#include <GameFramework/PlaySession.h>
#include <GameFramework/SaveGame.h>
#include <Input/InputBinding.h>
#include <Multiplayer/NetworkComponents.h>
#include <Navigation/Navigation.h>
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
    shockwave::registerAudioComponents();
    shockwave::registerGameUIComponents();
    shockwave::registerNavigationComponents();
    shockwave::setAudioSettings(
        {.hrtf = m_tool.audioHrtf >= 0 ? m_tool.audioHrtf == 1 : m_project.audioHrtf,
         .hrtfBilinear = m_project.audioHrtfHigh});
    shockwave::registerEffectsComponents();
    shockwave::registerAnimationComponents();
    shockwave::registerCircuitComponents();
    shockwave::net::registerNetworkComponents();
    const shockwave::GameModule& module = m_module;   // its types are registered already
    shockwave::createBuiltinAssets(engine().renderer(), engine().assets());
    if (module.createAssets) module.createAssets(engine());
    m_instance = shockwave::makeGameInstance(&module);
    m_instance->init(&engine());
    m_assets = std::make_unique<shockwave::AssetFolder>(engine().renderer(), engine().assets(), engine().jobs());
    applyOptimisationSettings();   // before importing (docs/Optimisation.md)
    std::vector<std::string> importProblems;
    for (const auto& change : m_assets->open(m_project.assetFolder)) {
        if (change.what == shockwave::AssetFolder::Change::What::Failed)
            importProblems.push_back(
                m_project.relative(m_project.assetFolder / change.name) + ": import failed: " + change.error);
    }
    if (m_tool.optimise) {   // --optimise
        m_exitCode = optimise();
        engine().quit();
        return;
    }
    m_ui = std::make_unique<shockwave::GameUI>(&engine());
    m_ui->setContentFolder(m_project.assetFolder);
    m_effects = std::make_unique<shockwave::Effects>(&engine());
    m_effects->setContentFolder(m_project.assetFolder);
    shockwave::prefabs().setup(m_project.assetFolder, &engine().assets());
    if (m_tool.mode != Tool::Mode::Play) {
        m_exitCode = m_tool.mode == Tool::Mode::Check         ? check(importProblems)
                     : m_tool.mode == Tool::Mode::BakeNavMesh ? bakeNavMeshes()
                     : m_tool.mode == Tool::Mode::AudioMix    ? audioMix()
                     : m_tool.mode == Tool::Mode::Animations  ? listAnimations()
                     : m_tool.mode == Tool::Mode::NodeSchema  ? nodeSchema()
                     : m_tool.mode == Tool::Mode::CircuitRun  ? runCircuit()
                     : m_tool.mode == Tool::Mode::CircuitDump ? dumpCircuit()
                                                              : schema();
        engine().quit();
        return;
    }
    if (!importProblems.empty()) throw std::runtime_error("Asset " + importProblems.front());
    if (!m_tool.scene.empty()) m_project.startupScene = m_tool.scene;
    // --load: a slot, or a file. Play starts in the scene the save came from, so its
    // GameMode runs, unless --scene says otherwise.
    fs::path loadFile;
    if (!m_tool.load.empty()) {
        loadFile = fs::path(m_tool.load).extension() == ".swsave" ? fs::absolute(m_tool.load)
                                                                  : saveFolder() / (m_tool.load + ".swsave");
        shockwave::SaveInfo info;
        std::string error;
        if (!shockwave::readSaveInfo(loadFile, info, error)) throw std::runtime_error(error);
        if (m_tool.scene.empty() && !info.scene.empty() && fs::exists(m_project.root / info.scene))
            m_project.startupScene = m_project.root / info.scene;
    }
    engine().renderer().setDebugView(m_tool.view);
    std::string error;
    // Tool modes may force presentation off; preserve their effective pacing.
    auto graphics = m_tool.graphics;
    graphics.vsync = engine().renderer().vsync();
    graphics.frameLimit = engine().frameLimit();
    graphics.lowLatency = engine().lowLatency();
    if (!engine().graphics().configure(m_tool.graphicsDefaults, m_tool.graphicsFile, error) ||
        !engine().graphics().apply(graphics, error))
        throw std::runtime_error(error);
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
    // A navigation mesh saved next to the scene saves baking it when Play starts.
    if (shockwave::loadSceneNavMesh(m_project.startupScene, error); !error.empty())
        SW_LOG(shockwave::LogNavigation, Warning, "{}; baking instead", error);
    m_world.each<const shockwave::AudioSource>([&](shockwave::Entity entity, const shockwave::AudioSource& source) {
        const auto problems = shockwave::validateAudioSource(source, engine().assets());
        if (!problems.empty()) throw std::runtime_error(problems.front());
        if (source.spatial && !m_world.has<shockwave::Transform>(entity))
            throw std::runtime_error("Spatial AudioSource requires Transform");
    });
    shockwave::FlyCamera::bindDefaultControls(engine().input());
    if (const auto bindings = shockwave::inputBindingsPath(m_project.root); std::filesystem::exists(bindings)) {
        if (!engine().input().loadBindingsFile(bindings, error)) throw std::runtime_error(error);
    }
    m_ui->setInputEnabled(true);
    m_ui->setPreview(m_tool.uiPreview);
    m_session = std::make_unique<shockwave::PlaySession>(m_world, engine().jobs(), *m_instance, &engine());
    m_session->setSaveFolder(saveFolder(), m_project.relative(m_project.startupScene));
    m_session->setGameCircuit(m_project.gameCircuitPath());
    if (!m_tool.netPreview.empty()) startNetPreview();
    m_session->start();
    if (!loadFile.empty()) {
        // Read now; the world is swapped in before the first tick.
        shockwave::SaveGames& saves = *m_session->saves();
        saves.loadFile(loadFile);
        saves.wait();
        if (!saves.lastError().empty()) throw std::runtime_error(saves.lastError());
        m_session->frame(0);
        SW_LOG(shockwave::LogGame, Info, "started from save {}", m_project.relative(loadFile));
    }
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

// Online services and the session's network, then GameMode::collectInput, before the ticks.
void Player::onPump(double rawFrameDt) {
    if (m_previewHost) {
        // The hub's clock moves with real time; the host pumps first, like a remote machine.
        m_previewHub->advance(uint64_t(std::max(rawFrameDt, 0.0) * 1e6));
        shockwave::NetworkFrame frame;
        frame.nowSeconds = engine().frameStartSeconds();
        frame.rawFrameDt = rawFrameDt;
        shockwave::pumpNetworkFrame(m_previewHostServices.get(), m_previewHost.get(), frame);
        shockwave::pumpNetworkFrame(engine(), m_previewClientServices.get(), m_session.get(), rawFrameDt);
        return;
    }
    shockwave::pumpNetworkFrame(engine(), m_services.get(), m_session.get(), rawFrameDt);
}

void Player::onUpdate(double dt) {
    if (m_previewHost) m_previewHost->update(dt);
    if (m_session) m_session->update(dt);
}

void Player::onFrame(double dt) {
    if (!m_session) return;
    for (const auto& change : m_assets->poll(dt)) {
        if (change.what == shockwave::AssetFolder::Change::What::Failed)
            SW_LOG(shockwave::LogEngine, Warning, "asset {}: {}", change.name, change.error);
    }
    // Prefab files saved while the game runs update every copy (hot reload).
    if (!shockwave::prefabs().poll(dt).empty()) shockwave::updatePrefabInstances(m_world);
    m_session->frame(dt);
    m_ui->update(m_world, dt);
    // --ui-nav: one menu step a frame, once documents have loaded and shown.
    if (const size_t step = m_uiNavFrame++; step >= 5 && step - 5 < m_tool.uiNav.size())
        m_ui->navigate(m_tool.uiNav[step - 5]);
    m_effects->update(m_world, dt);
    if (!shockwave::primaryCamera(m_world).valid()) m_camera.update(engine().input(), dt);
    if (!m_tool.screenshot.empty()) takeScreenshot();
    if (m_tool.benchmarkSeconds > 0) benchmark(dt);
    if (!m_tool.save.empty() && ++m_saveFrames == m_tool.saveFrame) saveAndQuit();
}

void Player::saveAndQuit() {
    shockwave::SaveGames& saves = *m_session->saves();
    if (saves.save(m_tool.save)) saves.wait();
    if (const std::string error = saves.lastError(); !error.empty()) {
        std::fprintf(stderr, "Player: %s\n", error.c_str());
        m_exitCode = 1;
    } else {
        std::printf(
            "Wrote %s (%zu entities, frame %d)\n", m_project.relative(saves.fileFor(m_tool.save)).c_str(),
            m_world.entityCount(), m_saveFrames);
    }
    engine().quit();
}

void Player::benchmark(double dt) {
    constexpr double kWarmUp = 1.0;   // shader caches, first uploads, clocks ramping up
    m_benchmarkTime += dt;
    if (!m_benchmarkMeasuring && m_benchmarkTime >= kWarmUp) {
        engine().frameStats().clear();
        m_benchmarkMeasuring = true;
        m_benchmarkTime = 0;
    }
    if (m_benchmarkMeasuring) {
        const shockwave::UIStats ui = m_ui->stats();   // the previous frame's
        m_uiUpdateMs += ui.updateMs;
        m_uiLayoutMs += ui.layoutMs;
        const shockwave::EffectsStats effects = m_effects->stats();
        m_effectsUpdateMs += effects.updateMs;
        m_effectsBuildMs += effects.buildMs;
        m_particles.waitMs += effects.waitMs;
        m_particles.particles += effects.particles;
        m_particles.peakParticles = std::max(m_particles.peakParticles, effects.particles);
        const shockwave::ParticleStats upload = engine().renderer().particleStats();
        m_particles.uploadMs += upload.uploadMs;
        m_particles.uploadBytes += static_cast<double>(upload.uploadBytes);
        for (const auto& pass : engine().renderer().passTimings())   // the latest frame the GPU finished
            if (pass.name == "particles") m_particles.gpuMs += pass.gpuMs;
        ++m_uiFrames;
    }
    if (m_benchmarkMeasuring && m_benchmarkTime >= m_tool.benchmarkSeconds) {
        reportBenchmark();
        engine().quit();
    }
}

void Player::reportBenchmark() {
    const shockwave::FrameSummary s = engine().frameStats().summary();
    shockwave::Renderer& renderer = engine().renderer();
    std::printf(
        "benchmark: %s, %zu frames over %.1f s\n"
        "gpu:          %s\n"
        "presentation: %s; %s\n"
        "frame limit:  %s; low latency %s\n"
        "average:      %.1f fps (%.3f ms)\n"
        "1%% low:       %.1f fps\n"
        "0.1%% low:     %.1f fps\n"
        "worst frame:  %.3f ms\n"
        "jitter:       %.3f ms\n"
        "cpu:          %.3f ms\n"
        "gpu time:     %.3f ms\n"
        "draws:        %u submitted, %u outside the view (culled)\n",
        m_project.relative(m_project.startupScene).c_str(), s.frames, m_tool.benchmarkSeconds, renderer.gpuName(),
        renderer.presentModeName(), renderer.presentTiming(),
        engine().frameLimit() > 0 ? (std::to_string(static_cast<int>(engine().frameLimit())) + " fps").c_str() : "none",
        engine().lowLatency() ? "on" : "off", s.averageFps, s.averageMs, s.low1Fps, s.low01Fps, s.maxMs, s.jitterMs,
        s.cpuMs, s.gpuMs, renderer.drawCount(), renderer.culledDraws());
    for (const auto& pass : renderer.passTimings())
        std::printf("  pass %-24s %.3f ms\n", pass.name.c_str(), static_cast<double>(pass.gpuMs));
    const double frames = static_cast<double>(std::max<size_t>(m_uiFrames, 1));
    const shockwave::UIStats ui = m_ui->stats();
    std::printf(
        "game ui cpu:  %.3f ms update + %.3f ms layout (%u documents, %u draws, %u vertices)\n", m_uiUpdateMs / frames,
        m_uiLayoutMs / frames, ui.visible, ui.drawCommands, ui.vertices);
    const shockwave::EffectsStats effects = m_effects->stats();
    std::printf(
        "effects cpu:  %.3f ms simulation (worker) + %.3f ms build (%u effects, %u particles, %u draws, %u vertices)\n",
        m_effectsUpdateMs / frames, m_effectsBuildMs / frames, effects.playing, effects.particles, effects.batches,
        effects.vertices);
    if (m_particles.peakParticles > 0) {
        // One line per stage, so a particle rewrite can be judged by where the time goes.
        std::printf(
            "particles:    %.0f average, %u peak (cap %d)\n"
            "  simulate    %.3f ms (worker; main thread waited %.3f ms)\n"
            "  build quads %.3f ms (main thread)\n"
            "  upload      %.3f ms (main thread, %.2f MB a frame)\n"
            "  gpu draw    %.3f ms (particles pass)\n",
            m_particles.particles / frames, m_particles.peakParticles, shockwave::Effects::kMaxParticles,
            m_effectsUpdateMs / frames, m_particles.waitMs / frames, m_effectsBuildMs / frames,
            m_particles.uploadMs / frames, m_particles.uploadBytes / frames / 1e6, m_particles.gpuMs / frames);
    }
    if (const shockwave::AnimationWorld* animation = m_session ? m_session->animation() : nullptr) {
        const shockwave::AnimationStats a = animation->stats();
        std::printf(
            "animation cpu: %.3f ms last frame (%u animators, %u joints; GPU: the skinning pass)\n",
            static_cast<double>(a.updateMs), a.animators, a.joints);
    }
    if (m_tool.benchmarkJson.empty()) return;
    std::ofstream json(m_tool.benchmarkJson, std::ios::trunc);
    auto escape = [](const std::string& text) {
        std::string out;
        for (char c : text) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out;
    };
    json << "{\n  \"scene\": \"" << escape(m_project.relative(m_project.startupScene)) << "\",\n"
         << "  \"gpu\": \"" << escape(renderer.gpuName()) << "\",\n"
         << "  \"present_mode\": \"" << escape(renderer.presentModeName()) << "\",\n"
         << "  \"present_timing\": \"" << escape(renderer.presentTiming()) << "\",\n"
         << "  \"frame_limit\": " << engine().frameLimit() << ",\n"
         << "  \"low_latency\": " << (engine().lowLatency() ? "true" : "false") << ",\n"
         << "  \"seconds\": " << m_tool.benchmarkSeconds << ",\n"
         << "  \"frames\": " << s.frames << ",\n"
         << "  \"average_fps\": " << s.averageFps << ",\n"
         << "  \"average_ms\": " << s.averageMs << ",\n"
         << "  \"low_1_percent_fps\": " << s.low1Fps << ",\n"
         << "  \"low_0_1_percent_fps\": " << s.low01Fps << ",\n"
         << "  \"worst_ms\": " << s.maxMs << ",\n"
         << "  \"jitter_ms\": " << s.jitterMs << ",\n"
         << "  \"cpu_ms\": " << s.cpuMs << ",\n"
         << "  \"gpu_ms\": " << s.gpuMs << ",\n"
         << "  \"draws\": " << renderer.drawCount() << ",\n"
         << "  \"culled_draws\": " << renderer.culledDraws() << ",\n"
         << "  \"ui_update_ms\": " << m_uiUpdateMs / frames << ",\n"
         << "  \"ui_layout_ms\": " << m_uiLayoutMs / frames << ",\n"
         << "  \"effects_update_ms\": " << m_effectsUpdateMs / frames << ",\n"
         << "  \"effects_build_ms\": " << m_effectsBuildMs / frames << ",\n"
         << "  \"particles\": {\"average\": " << m_particles.particles / frames
         << ", \"peak\": " << m_particles.peakParticles << ", \"cap\": " << shockwave::Effects::kMaxParticles
         << ", \"simulate_ms\": " << m_effectsUpdateMs / frames << ", \"wait_ms\": " << m_particles.waitMs / frames
         << ", \"build_ms\": " << m_effectsBuildMs / frames << ", \"upload_ms\": " << m_particles.uploadMs / frames
         << ", \"upload_bytes\": " << m_particles.uploadBytes / frames << ", \"gpu_ms\": " << m_particles.gpuMs / frames
         << "},\n"
         << "  \"passes\": [";
    const auto& passes = renderer.passTimings();
    for (size_t i = 0; i < passes.size(); ++i) {
        json << (i ? ", " : "") << "{\"name\": \"" << escape(passes[i].name) << "\", \"gpu_ms\": " << passes[i].gpuMs
             << "}";
    }
    json << "]\n}\n";
    std::printf("Wrote %s\n", m_tool.benchmarkJson.string().c_str());
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
    m_effects->render(renderer);
    if (m_session) m_ui->render(renderer);   // over the tonemapped scene, at native resolution
    if (m_tool.showNavMesh && m_session && m_session->navigation()) {
        const shockwave::NavMeshOverlay::Style style;
        m_navOverlay.drawMesh(renderer, m_session->navigation()->sharedNavMesh(), style);
        m_navOverlay.drawPaths(renderer, engine().assets(), *m_session->navigation(), m_world, style);
    }
}

void Player::onShutdown() {
    m_navOverlay.release(engine().renderer());
    if (m_tool.printFrame) std::printf("Render graph (last frame):\n%s", engine().renderer().frameGraph().c_str());
    if (m_session) {
        SW_LOG(shockwave::LogGame, Info, "simulation ticks: {}", m_session->ticks());
        m_session->stop();
    }
    m_session.reset();
    if (m_previewHost) m_previewHost->stop();
    m_previewHost.reset();
    if (m_instance) m_instance->shutdown();
    m_ui.reset();
    m_effects.reset();
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
