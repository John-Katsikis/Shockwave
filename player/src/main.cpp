#include <shockwave/Engine.h>
#include <shockwave/AssetFolder.h>
#include <shockwave/BuiltinAssets.h>
#include <shockwave/Gameplay.h>
#include <shockwave/Project.h>
#include <shockwave/SceneFile.h>
#include <shockwave/Systems.h>
#include <algorithm>
#include <cstdio>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;

// What the player was asked to do. Tools run in a hidden window and quit before the
// first frame; they need the renderer only to import assets the way the game does.
struct Tool {
    enum class Mode { Play, Check, Schema } mode = Mode::Play;
    std::vector<fs::path> scenes;   // --check: empty means every scene in the project
    fs::path schemaOut;             // --schema: "-" is standard output
    // Playing, these make runs reproducible and checkable without a person watching.
    fs::path scene;                 // --scene: instead of the project's startup scene
    fs::path screenshot;            // --screenshot: save frame N as a PNG, then quit
    int screenshotFrame = 30;       // --frames with --screenshot
    shockwave::DebugView view = shockwave::DebugView::Lit;   // --view
    bool printFrame = false;        // --print-frame: the render graph, at exit
};

class Player final : public shockwave::Game {
public:
    Player(shockwave::Project project, Tool tool) : m_project(std::move(project)), m_tool(std::move(tool)) {}
    int exitCode() const { return m_exitCode; }

    void onStart() override {
        const auto& module = shockwave::projectGameModule();
        if (module.registerTypes) module.registerTypes();
        shockwave::createBuiltinAssets(engine().renderer(), engine().assets());
        if (module.createAssets) module.createAssets(engine());
        m_assets = std::make_unique<shockwave::AssetFolder>(engine().renderer(), engine().assets(), engine().jobs());
        std::vector<std::string> importProblems;
        for (const auto& change : m_assets->open(m_project.assetFolder)) {
            if (change.what == shockwave::AssetFolder::Change::What::Failed)
                importProblems.push_back(m_project.relative(m_project.assetFolder / change.name) + ": import failed: " + change.error);
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
        m_session = std::make_unique<shockwave::GameSession>(m_world, engine().jobs(), module.create, &engine());
        m_session->start();
        if (!m_session->physics()->diagnostics().empty()) {
            const auto& problem=m_session->physics()->diagnostics().front();
            throw std::runtime_error("Physics setup failed for entity "+std::to_string(problem.entity.index)+": "+problem.message);
        }
        std::printf("Jolt physics: %u bodies, %u characters\n",m_session->physics()->bodyCount(),m_session->physics()->characterCount());
        std::printf("Player project: %s; module: %s; startup scene: %s\n", m_project.name.c_str(), m_project.gameModule.c_str(), m_project.startupScene.string().c_str());
    }
    void onUpdate(double dt) override { if (m_session) m_session->update(dt); }
    void onFrame(double dt) override {
        if (!m_session) return;
        m_session->frame(dt);
        if (!shockwave::primaryCamera(m_world).valid()) m_camera.update(engine().input(), dt);
        if (!m_tool.screenshot.empty()) takeScreenshot();
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
        if (m_tool.printFrame) std::printf("Render graph (last frame):\n%s", engine().renderer().frameGraph().c_str());
        if (!m_session) return;
        std::printf("Player simulation ticks: %llu\n", static_cast<unsigned long long>(m_session->ticks()));
        m_session->stop();
    }
private:
    // Asks for frame N, then reports and quits once the renderer has written it.
    void takeScreenshot() {
        auto& renderer = engine().renderer();
        ++m_frame;
        if (!m_screenshotRequested) {
            if (m_frame >= m_tool.screenshotFrame) {
                renderer.requestScreenshot(m_tool.screenshot.string());
                m_screenshotRequested = true;
            }
        } else if (!renderer.screenshotPending()) {
            if (renderer.screenshotResult().empty()) {
                std::printf("Wrote %s (%ux%u, %u lights)\n", m_tool.screenshot.string().c_str(), renderer.renderWidth(),
                            renderer.renderHeight(), renderer.lightCount());
            } else {
                std::fprintf(stderr, "Player: %s\n", renderer.screenshotResult().c_str());
                m_exitCode = 1;
            }
            engine().quit();
        }
    }

    // Loads each scene the way the game would and reports every problem as file:line.
    int check(const std::vector<std::string>& importProblems) {
        std::vector<std::string> problems = importProblems;
        std::vector<fs::path> scenes = m_tool.scenes;
        if (scenes.empty()) {
            std::error_code ec;
            for (const auto& entry : fs::recursive_directory_iterator(m_project.sceneFolder, ec)) {
                if (entry.is_regular_file(ec) && entry.path().extension() == ".swscene") scenes.push_back(entry.path());
            }
            if (std::find(scenes.begin(), scenes.end(), m_project.startupScene) == scenes.end())
                scenes.push_back(m_project.startupScene);
            std::sort(scenes.begin(), scenes.end());
        }
        const auto& module = shockwave::projectGameModule();
        for (const fs::path& scene : scenes) {
            const std::string shown = m_project.relative(scene);
            const size_t before = problems.size();
            // Problems name the file as the project does ("scenes/main.swscene:12: ...").
            auto report = [&](std::string problem) {
                if (problem.rfind(scene.string(), 0) == 0) problem.replace(0, scene.string().size(), shown);
                problems.push_back(std::move(problem));
            };
            shockwave::World world;
            std::string error;
            std::vector<std::string> warnings;
            shockwave::SceneFileHooks hooks;
            hooks.readHeader = [](const std::string&, std::istream&) {};   // old editor_camera lines
            if (!shockwave::loadScene(world, engine().assets(), scene.string(), error, hooks, &warnings)) {
                report(error);
                continue;
            }
            for (const std::string& warning : warnings) report(warning);

            int primaries = 0;
            world.each<const shockwave::Camera>([&](const shockwave::Camera& camera) { primaries += camera.primary; });
            if (primaries == 0) report(scene.string() + ": no entity has a primary Camera; the game has no view");
            if (primaries > 1) report(scene.string() + ": " + std::to_string(primaries) + " primary Cameras; keep one");

            // Lighting: a light needs a Transform to be placed, the renderer has a light
            // budget, and only the first Environment is used.
            auto entityName = [&](shockwave::Entity e) {
                const shockwave::Name* name = world.get<shockwave::Name>(e);
                return std::string(name ? name->value : "?");
            };
            size_t lights = 0;
            world.each<const shockwave::Light>([&](shockwave::Entity e, const shockwave::Light& light) {
                if (!world.get<shockwave::Transform>(e))
                    report(scene.string() + ": entity '" + entityName(e) + "': Light has no Transform; it won't shine");
                else if (light.enabled && light.intensity > 0.0f)
                    ++lights;
            });
            if (lights > shockwave::Renderer::kMaxLights)
                report(scene.string() + ": " + std::to_string(lights) + " enabled lights; only " +
                       std::to_string(shockwave::Renderer::kMaxLights) + " are drawn");
            int environments = 0;
            world.each<const shockwave::Environment>([&](shockwave::Entity e, const shockwave::Environment&) {
                if (++environments == 2)
                    report(scene.string() + ": entity '" + entityName(e) +
                           "': more than one Environment; only the first is used");
            });

            // Start the game on it, as Play does, to catch physics setup problems.
            try {
                shockwave::GameSession session(world, engine().jobs(), module.create, &engine());
                session.start();
                for (const auto& diagnostic : session.physics()->diagnostics()) {
                    const shockwave::Name* name = world.get<shockwave::Name>(diagnostic.entity);
                    report(scene.string() + ": entity '" + (name ? name->value : "?") + "': " + diagnostic.message);
                }
                session.stop();
            } catch (const std::exception& e) {
                report(scene.string() + ": gameplay failed to start: " + e.what());
            }
            if (problems.size() == before) std::printf("ok %s (%zu entities)\n", shown.c_str(), world.entityCount());
        }
        for (const std::string& problem : problems) std::printf("%s\n", problem.c_str());
        std::printf("%zu problem%s in %zu scene%s\n", problems.size(), problems.size() == 1 ? "" : "s", scenes.size(),
                    scenes.size() == 1 ? "" : "s");
        return problems.empty() ? 0 : 1;
    }

    int schema() {
        std::ostringstream out;
        shockwave::writeSceneSchema(out, engine().assets());
        out << "\n## Project manifest\n\n"
            << "`shockwave.swproject` sits at the project root; paths are relative to it and quoted.\n\n"
            << "```text\n"
            << "shockwave_project 1\n"
            << "name \"My Game\"\n"
            << "game_module \"" << m_project.gameModule << "\"      # must match the compiled gameplay library\n"
            << "startup_scene \"scenes/main.swscene\"   # must exist\n"
            << "assets \"assets\"                       # optional, default assets\n"
            << "scenes \"scenes\"                       # optional, default scenes\n"
            << "theme \"themes/default.swtheme\"        # optional\n"
            << "tick_rate 60                           # simulation updates per second\n"
            << "window_size 1280 720\n"
            << "vsync true\n"
            << "```\n\n"
            << "Unknown or repeated keys are errors. `player --check` validates the manifest too.\n";
        if (m_tool.schemaOut == "-") {
            std::fputs(out.str().c_str(), stdout);
            return 0;
        }
        std::ofstream file(m_tool.schemaOut, std::ios::trunc);
        if (!(file << out.str())) {
            std::fprintf(stderr, "Player: can't write %s\n", m_tool.schemaOut.string().c_str());
            return 1;
        }
        std::printf("Wrote %s\n", m_tool.schemaOut.string().c_str());
        return 0;
    }

    shockwave::Project m_project;
    Tool m_tool;
    int m_exitCode = 0;
    int m_frame = 0;
    bool m_screenshotRequested = false;
    shockwave::World m_world;
    shockwave::FlyCamera m_camera;
    std::unique_ptr<shockwave::AssetFolder> m_assets;
    std::unique_ptr<shockwave::GameSession> m_session;
};
}
int main(int argc, char** argv) {
    try {
        std::filesystem::path manifest;
        int frames = 0;
        Tool tool;
        std::pair<int, int> size{0, 0};
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--project" && i + 1 < argc) manifest = argv[++i];
            else if (arg == "--frames" && i + 1 < argc) {
                const std::string value = argv[++i];
                const auto [end, ec] = std::from_chars(value.data(), value.data()+value.size(), frames);
                if (ec != std::errc{} || end != value.data()+value.size() || frames <= 0)
                    throw std::runtime_error("--frames requires a positive integer");
            } else if (arg == "--check") {
                tool.mode = Tool::Mode::Check;
            } else if (arg == "--schema" && i + 1 < argc) {
                tool.mode = Tool::Mode::Schema;
                tool.schemaOut = argv[++i];
            } else if (arg == "--scene" && i + 1 < argc) {
                tool.scene = std::filesystem::absolute(argv[++i]).lexically_normal();
            } else if (arg == "--screenshot" && i + 1 < argc) {
                tool.screenshot = argv[++i];
            } else if (arg == "--view" && i + 1 < argc) {
                const std::string view = argv[++i];
                if (view == "lit") tool.view = shockwave::DebugView::Lit;
                else if (view == "unlit") tool.view = shockwave::DebugView::Unlit;
                else if (view == "normals") tool.view = shockwave::DebugView::Normals;
                else if (view == "lighting") tool.view = shockwave::DebugView::Lighting;
                else throw std::runtime_error("--view takes lit, unlit, normals or lighting");
            } else if (arg == "--size" && i + 1 < argc) {
                const std::string value = argv[++i];
                const size_t x = value.find('x');
                int w = 0, h = 0;
                if (x == std::string::npos ||
                    std::from_chars(value.data(), value.data() + x, w).ec != std::errc{} ||
                    std::from_chars(value.data() + x + 1, value.data() + value.size(), h).ec != std::errc{} ||
                    w < 16 || h < 16)
                    throw std::runtime_error("--size takes WIDTHxHEIGHT, like 1280x720");
                size = {w, h};
            } else if (arg == "--print-frame") {
                tool.printFrame = true;
            } else if (arg == "--help") {
                std::puts("player [project.swproject] [--project path] [--scene file.swscene] [--frames N]\n"
                          "       [--size WxH] [--view lit|unlit|normals|lighting] [--print-frame]\n"
                          "player --screenshot out.png [--scene file] [--frames N] [--size WxH] [--view ...]\n"
                          "                                    play N frames (default 30), save the last, quit\n"
                          "player --check [scene.swscene...]   validate the project and scenes; exit 1 on any problem\n"
                          "player --schema file.md             write the scene format reference (- for stdout)");
                return 0;
            } else if (!arg.empty() && arg[0] != '-') {
                if (tool.mode == Tool::Mode::Check && std::filesystem::path(arg).extension() == ".swscene")
                    tool.scenes.push_back(std::filesystem::absolute(arg).lexically_normal());
                else manifest = arg;
            }
            else throw std::runtime_error("Unknown or incomplete option: " + arg);
        }
        if (manifest.empty()) {
            // The project around the scene given, else the working folder's.
            const std::filesystem::path near = !tool.scenes.empty() ? tool.scenes.front()
                                               : !tool.scene.empty() ? tool.scene
                                                                     : std::filesystem::current_path();
            const auto found = shockwave::findProjectFile(near);
            manifest = found ? *found : "shockwave.swproject";
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
        if (size.first) config.window.width = size.first, config.window.height = size.second;
        if (!tool.screenshot.empty()) {
            // Runs hidden until the frame is saved; maxFrames is only a safety net.
            if (frames) tool.screenshotFrame = frames;
            config.maxFrames = tool.screenshotFrame + 120;
            config.window.hidden = true;
            config.window.vsync = false;
        }
        if (tool.mode != Tool::Mode::Play) {
            config.window.hidden = true;
            config.window.vsync = false;
        }
        shockwave::Engine engine(config);
        Player player(std::move(project), std::move(tool));
        engine.run(player);
        if (player.exitCode() != 0) return player.exitCode();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Player: %s\n", e.what()); return 1;
    }
    return 0;
}
