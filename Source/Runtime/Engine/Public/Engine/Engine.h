#pragma once

#include <Assets/Assets.h>
#include <Assets/BuiltinAssets.h>
#include <Core/Jobs.h>
#include <Engine/Camera.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Engine/SceneFile.h>
#include <Engine/Schedule.h>
#include <Engine/Systems.h>
#include <Engine/World.h>
#include <Input/Input.h>
#include <Platform/Window.h>
#include <Projects/Project.h>
#include <Renderer/Mesh.h>
#include <Renderer/Renderer.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

struct ImFont;

namespace shockwave {

class Engine;

// A game plugs into the engine by overriding these hooks. They run on the main
// thread; use engine().jobs() to spread work across cores.
class Game {
public:
    virtual ~Game() = default;
    virtual void onStart() {}
    // Simulation: runs at a fixed rate (EngineConfig::tickRate) no matter the frame rate,
    // so gameplay and physics behave the same at 30 fps and 500 fps.
    virtual void onUpdate(double dt) { (void)dt; }
    // Once per rendered frame with the real elapsed time: cameras, UI, effects.
    virtual void onFrame(double frameDt) { (void)frameDt; }
    // alpha (0..1) is how far we are between the last two ticks; blend previous and
    // current state with it so motion is smooth at any frame rate.
    virtual void onRender(Renderer& renderer, double alpha) {
        (void)renderer;
        (void)alpha;
    }
    // Dear ImGui calls go here when EngineConfig::gui is on. Runs once per rendered
    // frame, before onRender, and the UI is drawn on top of the scene.
    virtual void onGui() {}
    // The user closed the window. Return false to keep running (to ask about unsaved
    // work, say), then call engine().quit() when ready.
    virtual bool onQuitRequested() { return true; }
    virtual void onShutdown() {}

protected:
    Engine& engine() { return *m_engine; }

private:
    friend class Engine;
    Engine* m_engine = nullptr;
};

struct EngineConfig {
    WindowDesc window;
    double tickRate = 60.0;   // simulation updates per second
    uint32_t workerThreads = 0;   // 0 = one per hardware thread, minus the main thread
    int maxFrames = 0;   // 0 = run until closed; >0 quits after N frames (handy for smoke tests)
    bool showFpsInTitle = true;
    bool quitOnEscape = true;
    bool gui = false;   // Dear ImGui on top of the scene; see Game::onGui
    // The UI theme, looked for in the working folder, then next to the executable. Saved
    // changes to it apply straight away. Empty, or missing, uses the built-in theme.
    std::string guiTheme = "themes/default.swtheme";
};

class Engine {
public:
    explicit Engine(const EngineConfig& config);
    ~Engine();

    void run(Game& game);
    void quit() { m_running = false; }

    JobSystem& jobs() { return *m_jobs; }
    Window& window() { return *m_window; }
    Renderer& renderer() { return *m_renderer; }
    Input& input() { return *m_input; }
    Assets& assets() { return *m_assets; }

    // Switches the UI theme (a .swtheme path, found like EngineConfig::guiTheme) from the
    // next frame on. Main thread.
    void setGuiTheme(const std::string& path);
    const std::string& guiThemePath() const { return m_themePath; }   // empty: built-in theme

    double fps() const { return m_fps; }
    double tickRate() const { return m_config.tickRate; }
    // Changes the simulation rate from the next frame on. Main thread.
    void setTickRate(double ticksPerSecond);
    bool guiEnabled() const { return m_config.gui; }

private:
    void initGui();
    void shutdownGui();
    void loadGuiTheme(bool reload);
    void pollGuiTheme(double frameDt);

    EngineConfig m_config;
    std::unique_ptr<JobSystem> m_jobs;
    std::unique_ptr<Window> m_window;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<Input> m_input;
    std::unique_ptr<Assets> m_assets;
    bool m_running = false;
    double m_fps = 0.0;

    // UI theme hot reload
    std::string m_themePath;
    std::filesystem::file_time_type m_themeTime{};
    double m_themePollTimer = 0.0;
    bool m_themeChangeRequested = false;
    float m_dpiScale = 1.0f;
    std::unordered_map<std::string, ImFont*> m_fonts;
};

}   // namespace shockwave
