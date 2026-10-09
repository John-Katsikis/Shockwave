#pragma once

#include <shockwave/Camera.h>
#include <shockwave/Input.h>
#include <shockwave/Jobs.h>
#include <shockwave/Mesh.h>
#include <shockwave/Renderer.h>
#include <shockwave/Window.h>

#include <memory>

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
    virtual void onRender(Renderer& renderer, double alpha) { (void)renderer; (void)alpha; }
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
    double tickRate = 60.0;     // simulation updates per second
    uint32_t workerThreads = 0; // 0 = one per hardware thread, minus the main thread
    int maxFrames = 0;          // 0 = run until closed; >0 quits after N frames (handy for smoke tests)
    bool showFpsInTitle = true;
    bool quitOnEscape = true;
    bool gui = false;           // Dear ImGui on top of the scene; see Game::onGui
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

    double fps() const { return m_fps; }
    bool guiEnabled() const { return m_config.gui; }

private:
    void initGui();
    void shutdownGui();

    EngineConfig m_config;
    std::unique_ptr<JobSystem> m_jobs;
    std::unique_ptr<Window> m_window;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<Input> m_input;
    bool m_running = false;
    double m_fps = 0.0;
};

} // namespace shockwave
