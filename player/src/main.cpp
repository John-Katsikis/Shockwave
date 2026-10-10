#include <shockwave/Engine.h>
#include <shockwave/AssetFolder.h>
#include <shockwave/BuiltinAssets.h>
#include <shockwave/Gameplay.h>
#include <shockwave/Project.h>
#include <shockwave/Systems.h>
#include <cstdio>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace {
class Player final : public shockwave::Game {
public:
    explicit Player(shockwave::Project project) : m_project(std::move(project)) {}
    void onStart() override {
        const auto& module = shockwave::projectGameModule();
        if (module.registerTypes) module.registerTypes();
        shockwave::createBuiltinAssets(engine().renderer(), engine().assets());
        if (module.createAssets) module.createAssets(engine());
        m_assets = std::make_unique<shockwave::AssetFolder>(engine().renderer(), engine().assets(), engine().jobs());
        for (const auto& change : m_assets->open(m_project.assetFolder)) {
            if (change.what == shockwave::AssetFolder::Change::What::Failed)
                throw std::runtime_error("Asset import failed: " + change.name + ": " + change.error);
        }
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
        m_session = std::make_unique<shockwave::GameSession>(m_world, engine().jobs(), module.create, &engine());
        m_session->start();
        if (!m_session->physics()->diagnostics().empty()) {
            const auto& problem=m_session->physics()->diagnostics().front();
            throw std::runtime_error("Physics setup failed for entity "+std::to_string(problem.entity.index)+": "+problem.message);
        }
        std::printf("Jolt physics: %u bodies, %u characters\n",m_session->physics()->bodyCount(),m_session->physics()->characterCount());
        std::printf("Player project: %s; module: %s; startup scene: %s\n", m_project.name.c_str(), m_project.gameModule.c_str(), m_project.startupScene.string().c_str());
    }
    void onUpdate(double dt) override { m_session->update(dt); }
    void onFrame(double dt) override {
        m_session->frame(dt);
        if (!shockwave::primaryCamera(m_world).valid()) m_camera.update(engine().input(), dt);
    }
    void onRender(shockwave::Renderer& renderer, double alpha) override {
        // The game looks through its primary Camera, wherever gameplay has moved it.
        glm::mat4 view, projection;
        if (shockwave::cameraMatrices(m_world, shockwave::primaryCamera(m_world), renderer.aspectRatio(), view, projection)) {
            renderer.setCamera(view, projection);
        } else {
            renderer.setCamera(m_camera.view(), m_camera.projection(renderer.aspectRatio()));
        }
        shockwave::renderWorld(m_world, renderer, engine().jobs(), alpha, &engine().assets());
    }
    void onShutdown() override {
        std::printf("Player simulation ticks: %llu\n", static_cast<unsigned long long>(m_session->ticks()));
        m_session->stop();
    }
private:
    shockwave::Project m_project;
    shockwave::World m_world;
    shockwave::FlyCamera m_camera;
    std::unique_ptr<shockwave::AssetFolder> m_assets;
    std::unique_ptr<shockwave::GameSession> m_session;
};
}
int main(int argc, char** argv) {
    try {
        std::filesystem::path manifest = "shockwave.swproject";
        int frames = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--project" && i + 1 < argc) manifest = argv[++i];
            else if (arg == "--frames" && i + 1 < argc) {
                const std::string value = argv[++i];
                const auto [end, ec] = std::from_chars(value.data(), value.data()+value.size(), frames);
                if (ec != std::errc{} || end != value.data()+value.size() || frames <= 0)
                    throw std::runtime_error("--frames requires a positive integer");
            } else if (arg == "--help") {
                std::puts("player [project.swproject] [--project path] [--frames N]"); return 0;
            } else if (!arg.empty() && arg[0] != '-') manifest = arg;
            else throw std::runtime_error("Unknown or incomplete option: " + arg);
        }
        shockwave::Project project;
        std::string error;
        if (!shockwave::loadProject(manifest, project, error)) throw std::runtime_error(error);
        if (project.gameModule != shockwave::projectGameModule().id)
            throw std::runtime_error("Project game_module does not match the compiled gameplay library");
        shockwave::EngineConfig config;
        config.window = {project.name, project.width, project.height, project.vsync};
        config.tickRate = project.tickRate;
        config.maxFrames = frames;
        shockwave::Engine engine(config);
        Player player(std::move(project));
        engine.run(player);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Player: %s\n", e.what()); return 1;
    }
    return 0;
}
