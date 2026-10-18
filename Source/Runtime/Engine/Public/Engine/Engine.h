#pragma once

#include <Assets/Assets.h>
#include <Assets/BuiltinAssets.h>
#include <Core/FramePacing.h>
#include <Core/Jobs.h>
#include <Core/Plugin.h>
#include <Engine/Camera.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Engine/GraphicsSettings.h>
#include <Engine/SceneFile.h>
#include <Engine/Schedule.h>
#include <Engine/Systems.h>
#include <Engine/World.h>
#include <Input/Input.h>
#include <Platform/Window.h>
#include <Projects/Project.h>
#include <Renderer/Mesh.h>
#include <Renderer/Renderer.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

struct ImFont;

namespace shockwave {

class Engine;

// The program the engine runs: the editor, the player or a sandbox overrides these
// hooks (game code itself goes in a GameMode; see GameFramework). They run on the main
// thread; use engine().jobs() to spread work across cores.
class Application {
public:
    virtual ~Application() = default;
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
    bool gui = false;   // Dear ImGui on top of the scene; see Application::onGui
    // GUI windows can be dragged out of the main window to anywhere on the desktop, each
    // becoming a window of its own (Dear ImGui multi-viewports). Needs a platform that can
    // place windows: on a Wayland session the engine runs through XWayland for this.
    bool guiViewports = false;
    // Frame pacing (see Engine::setFrameLimit and setLowLatency).
    double frameLimit = 0.0;   // frames per second; 0 = no limit beyond vsync
    bool lowLatency = false;
    // The UI theme, looked for in the working folder, then next to the executable. Saved
    // changes to it apply straight away. Empty, or missing, uses the built-in theme.
    std::string guiTheme = "themes/default.swtheme";
    // The renderer plugin to start with ("vulkan", "null"; see Renderer::availableBackends).
    std::string renderer = "vulkan";
    // Let the renderer be swapped or reloaded while running (Engine::switchRenderer). Keeps
    // a CPU copy of every mesh and texture. The editor turns it on.
    bool swappableRenderer = false;
};

class Engine {
public:
    explicit Engine(const EngineConfig& config);
    ~Engine();

    void run(Application& app);
    void quit() { m_running = false; }

    JobSystem& jobs() { return *m_jobs; }
    Window& window() { return *m_window; }
    Renderer& renderer() { return *m_renderer; }
    Graphics& graphics() { return m_graphics; }
    Input& input() { return *m_input; }
    Assets& assets() { return *m_assets; }
    void setMasterVolume(float volume);   // any thread; 0..1
    float masterVolume() const { return m_masterVolume.load(); }

    // Switches the UI theme (a .swtheme path, found like EngineConfig::guiTheme) from the
    // next frame on. Main thread.
    void setGuiTheme(const std::string& path);
    const std::string& guiThemePath() const { return m_themePath; }   // empty: built-in theme

    double fps() const { return m_fps; }

    // ---- Frame pacing ----
    // Caps the frame rate precisely (sleep, then spin to the deadline). 0 = no cap beyond
    // vsync. A cap just under the display rate with vsync off gives steady frames with low
    // latency. Main thread.
    void setFrameLimit(double fps);
    double frameLimit() const { return m_limiter.targetFps(); }
    // Low latency: before reading input, wait until the previous frame is on screen, so no
    // frames queue up between input and display (like Nvidia Reflex). Costs some throughput
    // when the GPU is the bottleneck. Main thread.
    void setLowLatency(bool lowLatency);
    bool lowLatency() const { return m_lowLatency; }
    // The last few thousand frames: CPU, GPU and present timing, with 1% and 0.1% lows.
    const FrameStats& frameStats() const { return m_frameStats; }
    FrameStats& frameStats() { return m_frameStats; }   // to clear it, say after loading
    double tickRate() const { return m_config.tickRate; }
    // Main thread, during onFrame/onRender: the same blend factor renderWorld uses.
    // Frame-driven cameras can follow interpolated physics without lagging mouse input.
    double interpolationAlpha() const { return m_interpolationAlpha; }
    // Changes the simulation rate from the next frame on. Main thread.
    void setTickRate(double ticksPerSecond);
    bool guiEnabled() const { return m_config.gui; }

    // ---- Renderer plugins (docs/Plugins.md) ----
    // Swaps the renderer for another plugin at the start of the next frame, or reloads the
    // current one from disk (after rebuilding it or its shaders). Needs
    // EngineConfig::swappableRenderer. The outcome is logged and kept in rendererStatus().
    void switchRenderer(const std::string& name);
    void reloadRenderer() { switchRenderer(m_renderer->backendName()); }
    // Reload the renderer by itself whenever its library is rebuilt.
    void setAutoReloadRenderer(bool autoReload);
    bool autoReloadRenderer() const { return m_autoReloadRenderer; }
    struct RendererStatus {
        uint64_t changes = 0;   // counts swaps and reloads, successful or not
        bool ok = true;
        std::string message;
    };
    const RendererStatus& rendererStatus() const { return m_rendererStatus; }

private:
    void initGui();
    void shutdownGui();
    void loadGuiTheme(bool reload);
    void pollGuiTheme(double frameDt);

    Graphics m_graphics{this};
    EngineConfig m_config;
    std::unique_ptr<JobSystem> m_jobs;
    std::unique_ptr<Window> m_window;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<Input> m_input;
    std::unique_ptr<Assets> m_assets;
    std::atomic<float> m_masterVolume{1};
    bool m_running = false;
    double m_fps = 0.0;
    double m_interpolationAlpha = 1.0;
    FrameLimiter m_limiter;
    FrameTimeSmoother m_smoother;
    FrameStats m_frameStats;
    bool m_lowLatency = false;
    void updatePacingPeriod();
    bool applyRendererChange();   // true when the renderer changed (or failed to)

    // Renderer swapping
    std::string m_pendingRenderer;
    bool m_autoReloadRenderer = false;
    RebuildWatcher m_rendererWatcher;
    RendererStatus m_rendererStatus;

    // UI theme hot reload
    std::string m_themePath;
    std::filesystem::file_time_type m_themeTime{};
    double m_themePollTimer = 0.0;
    bool m_themeChangeRequested = false;
    float m_dpiScale = 1.0f;
    std::unordered_map<std::string, ImFont*> m_fonts;
};

}   // namespace shockwave
