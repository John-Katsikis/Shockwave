#include <Engine/Engine.h>

#include <Core/Log.h>
#include <Core/Profile.h>
#include <UI/Gui.h>

#include <imgui_impl_sdl3.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

namespace shockwave {

namespace {

// Wayland doesn't let programs place their own windows, so Dear ImGui can't put a panel
// where it was dropped. When windows must leave the main one, use X11 through XWayland
// instead, unless the user picked a video driver themselves.
bool preferX11ForGuiViewports() {
#ifdef __linux__
    if (SDL_getenv("SDL_VIDEO_DRIVER") || !SDL_getenv("WAYLAND_DISPLAY") || !SDL_getenv("DISPLAY")) return false;
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
    return true;
#else
    return false;
#endif
}

}   // namespace

Engine::Engine(const EngineConfig& config) : m_config(config) {
    // Closing the main window is handled as a quit request below; SDL's own quit is left
    // for signals (Ctrl+C), so the request isn't made twice.
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    bool initialized = false;
    if (m_config.gui && m_config.guiViewports && preferX11ForGuiViewports()) {
        initialized = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        if (initialized) {
            SW_LOG(LogEngine, Info, "Wayland session: using XWayland so panels can be dragged out of the main window");
        } else {   // no XWayland, or SDL built without X11: stay on Wayland
            SW_LOG(LogEngine, Warning, "XWayland unavailable ({}): panels stay inside the main window", SDL_GetError());
            SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
        }
    }
    if (!initialized) initialized = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    // A hidden window needs no display: tools (player --check) still run on a headless
    // machine, through SDL's offscreen driver.
    if (!initialized && m_config.window.hidden && !SDL_getenv("SDL_VIDEO_DRIVER")) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
        initialized = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    }
    if (!initialized) throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    m_jobs = std::make_unique<JobSystem>(m_config.workerThreads);
    m_window = std::make_unique<Window>(m_config.window);
    m_renderer = std::make_unique<Renderer>(
        *m_window, *m_jobs,
        RendererOptions{
            .backend = m_config.renderer, .vsync = m_config.window.vsync, .swappable = m_config.swappableRenderer});
    if (m_config.gui && !(m_renderer->backendFlags() & kRendererDrawsGui))
        throw std::runtime_error("the " + m_config.renderer + " renderer can't draw the editor's interface");
    m_input = std::make_unique<Input>(m_window.get());
    m_assets = std::make_unique<Assets>();
    setFrameLimit(m_config.frameLimit);
    setLowLatency(m_config.lowLatency);
    if (m_config.gui) initGui();
    SW_LOG(
        LogEngine, Info, "rendering with {} on {}, {} worker threads", m_renderer->backendName(), m_renderer->gpuName(),
        m_jobs->workerCount());
}

Engine::~Engine() {
    if (m_config.gui) shutdownGui();
    m_input.reset();
    m_renderer.reset();
    m_window.reset();
    m_jobs.reset();
    SDL_Quit();
}

void Engine::setMasterVolume(float value) {
    if (std::isfinite(value)) m_masterVolume.store(std::clamp(value, 0.0f, 1.0f));
}

void Engine::initGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    if (m_config.guiViewports) io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    // Layout and window state persist in the user's config folder
    // (~/.local/share/Shockwave/<app>/ on Linux), not wherever the app was started from.
    static std::string iniPath;
    if (char* pref = SDL_GetPrefPath("Shockwave", m_config.window.title.c_str())) {
        iniPath = std::string(pref) + "imgui.ini";
        SDL_free(pref);
        io.IniFilename = iniPath.c_str();
    } else {
        io.IniFilename = nullptr;
    }

    // ImGui works in window coordinates. Where those are points (macOS, Wayland), the
    // screen's pixel density is already applied through the framebuffer scale, so only
    // the rest of the display scale is left for sizes; elsewhere density is 1.
    const float density = SDL_GetWindowPixelDensity(m_window->handle());
    m_dpiScale = SDL_GetWindowDisplayScale(m_window->handle()) / (density > 0.0f ? density : 1.0f);
    if (m_dpiScale <= 0.0f) m_dpiScale = 1.0f;
    loadGuiTheme(false);

    ImGui_ImplSDL3_InitForVulkan(m_window->handle());
    m_renderer->initGui([this] {
        ImGui_ImplSDL3_Shutdown();
        ImGui_ImplSDL3_InitForVulkan(m_window->handle());
    });
    if (m_config.guiViewports && !(io.BackendFlags & ImGuiBackendFlags_PlatformHasViewports)) {
        io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
        SW_LOG(
            LogEngine, Warning, "the {} video driver can't place windows: panels stay inside the main window",
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "current");
    }
}

namespace {

// A path as given if it exists from the working folder, else next to the executable.
std::string findFile(const std::string& path) {
    if (path.empty()) return {};
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return path;
    const std::string beside = std::string(SDL_GetBasePath()) + path;
    if (std::filesystem::exists(beside, ec)) return beside;
    return {};
}

}   // namespace

// Loads the theme file over the built-in theme and applies it, along with its font.
// On a reload, a broken file leaves the current look alone and says what's wrong.
void Engine::loadGuiTheme(bool reload) {
    gui::Theme theme;
    if (!reload) m_themePath = findFile(m_config.guiTheme);
    if (!m_themePath.empty()) {
        std::string error;
        std::error_code ec;
        m_themeTime = std::filesystem::last_write_time(m_themePath, ec);
        if (!gui::loadTheme(m_themePath, theme, error)) {
            SW_LOG(LogEngine, Warning, "UI theme not applied: {}", error);
            if (reload) return;
            theme = gui::Theme();
        } else if (reload) {
            SW_LOG(LogEngine, Info, "reloaded UI theme {}", m_themePath);
        }
    } else if (!m_config.guiTheme.empty()) {
        SW_LOG(LogEngine, Warning, "{} not found, using the built-in UI theme", m_config.guiTheme);
    }

    // Fonts load once per file; ImGui 1.92 renders any size from one font on demand.
    ImGuiIO& io = ImGui::GetIO();
    const std::string fontPath = findFile(theme.font);
    if (!fontPath.empty()) {
        ImFont*& font = m_fonts[fontPath];
        if (!font) font = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), theme.fontSize);
        if (font) io.FontDefault = font;
    } else if (io.Fonts->Fonts.empty()) {
        io.Fonts->AddFontDefault();
    }
    gui::applyTheme(theme, m_dpiScale);
}

// Checks the theme file twice a second and reapplies it when it changes.
void Engine::setGuiTheme(const std::string& path) {
    m_config.guiTheme = path;
    m_themeChangeRequested = true;
}

void Engine::pollGuiTheme(double frameDt) {
    if (m_themeChangeRequested) {
        m_themeChangeRequested = false;
        loadGuiTheme(false);
        return;
    }
    if (m_themePath.empty()) return;
    m_themePollTimer += frameDt;
    if (m_themePollTimer < 0.5) return;
    m_themePollTimer = 0.0;
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(m_themePath, ec);
    if (!ec && time != m_themeTime) loadGuiTheme(true);
}

void Engine::setTickRate(double ticksPerSecond) {
    if (std::isfinite(ticksPerSecond) && ticksPerSecond >= 1.0) m_config.tickRate = ticksPerSecond;
}

void Engine::shutdownGui() {
    m_renderer->shutdownGui();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void Engine::setFrameLimit(double fps) {
    m_limiter.setTargetFps(fps);
    updatePacingPeriod();
}

void Engine::setLowLatency(bool lowLatency) {
    m_lowLatency = lowLatency;
    m_renderer->setLowLatency(lowLatency);
}

// The period frames should arrive at, so the smoother can snap to it: the frame cap's, or
// with vsync the display's, whichever is slower.
void Engine::updatePacingPeriod() {
    double period = m_limiter.targetFps() > 0.0 ? 1.0 / m_limiter.targetFps() : 0.0;
    if (m_renderer && m_renderer->vsync()) {
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(m_window->handle()));
        if (mode && mode->refresh_rate > 0.0f) period = std::max(period, 1.0 / mode->refresh_rate);
    }
    m_smoother.setPeriod(period);
}

void Engine::switchRenderer(const std::string& name) { m_pendingRenderer = name; }

void Engine::setAutoReloadRenderer(bool autoReload) {
    m_autoReloadRenderer = autoReload && m_config.swappableRenderer;
    m_rendererWatcher.watch(m_autoReloadRenderer ? m_renderer->backendFile() : std::filesystem::path());
}

// Between frames: nothing is being recorded, and the GUI frame hasn't begun.
bool Engine::applyRendererChange() {
    if (m_autoReloadRenderer && m_rendererWatcher.poll(static_cast<double>(SDL_GetTicksNS()) * 1e-9) &&
        m_pendingRenderer.empty()) {
        SW_LOG(LogRender, Info, "{} was rebuilt: reloading it", m_rendererWatcher.file().filename().string());
        m_pendingRenderer = m_renderer->backendName();
    }
    if (m_pendingRenderer.empty()) return false;
    const std::string name = std::exchange(m_pendingRenderer, {});
    const bool reload = name == m_renderer->backendName();
    std::string error;
    const Uint64 start = SDL_GetPerformanceCounter();
    const bool ok = m_renderer->switchBackend(name, error);
    const double ms = static_cast<double>(SDL_GetPerformanceCounter() - start) * 1000.0 /
                      static_cast<double>(SDL_GetPerformanceFrequency());
    ++m_rendererStatus.changes;
    m_rendererStatus.ok = ok;
    if (ok) {
        char text[256];
        std::snprintf(
            text, sizeof(text), "%s the %s renderer in %.0f ms", reload ? "Reloaded" : "Switched to", name.c_str(), ms);
        m_rendererStatus.message = text;
        SW_LOG(LogRender, Info, "{}", m_rendererStatus.message);
        m_frameStats.clear();   // the swap's stall isn't a frame
    } else {
        m_rendererStatus.message = (reload ? "Couldn't reload the renderer: " : "Couldn't switch renderers: ") + error;
        SW_LOG(LogRender, Error, "{}", m_rendererStatus.message);
    }
    // A failed build isn't retried until it's rebuilt again.
    if (m_autoReloadRenderer) m_rendererWatcher.watch(m_renderer->backendFile());
    return true;
}

void Engine::run(Application& app) {
    // Fixed-timestep loop: simulation ticks at tickRate, rendering runs as fast as vsync allows.
    const double maxFrameTime = 0.25;   // avoid the "spiral of death" after a long stall
    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());

    double accumulator = 0.0;
    Uint64 previous = SDL_GetPerformanceCounter();
    int frames = 0;

    double fpsTimer = 0.0;
    int fpsFrames = 0;

    m_running = true;
    app.m_engine = this;
    app.onStart();

    while (m_running) {
        // The swap's stall isn't counted as frame time.
        if ((!m_pendingRenderer.empty() || m_autoReloadRenderer) && applyRendererChange())
            previous = SDL_GetPerformanceCounter();
        // Pacing first, input right after: the less time between reading input and the
        // frame reaching the screen, the more responsive it feels.
        {
            SW_PROFILE_SCOPE("Frame limiter");
            m_limiter.wait();
        }
        const float presentMs = m_lowLatency ? m_renderer->waitForPresent() : 0.0f;
        const Uint64 frameStart = SDL_GetPerformanceCounter();
        SW_PROFILE_SCOPE("Frame");

        m_input->beginFrame();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (m_config.gui) ImGui_ImplSDL3_ProcessEvent(&event);
            m_input->handleEvent(event);
            switch (event.type) {
            case SDL_EVENT_QUIT:
                if (app.onQuitRequested()) m_running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (m_config.quitOnEscape && event.key.scancode == SDL_SCANCODE_ESCAPE) m_running = false;
                break;
            // Panels dragged out of the main window are windows too: only the main one's
            // events concern the engine; Dear ImGui handles the others.
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                // Closing the main window quits, even with panel windows still open.
                if (event.window.windowID == SDL_GetWindowID(m_window->handle()) && app.onQuitRequested())
                    m_running = false;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (event.window.windowID == SDL_GetWindowID(m_window->handle())) m_renderer->onResize();
                break;
            default: break;
            }
        }

        // Start of the previous frame to the start of this one.
        const double rawFrameTime = static_cast<double>(frameStart - previous) / freq;
        previous = frameStart;
        double frameTime = std::min(m_smoother.smooth(rawFrameTime), maxFrameTime);
        accumulator += frameTime;
        m_input->update(frameTime);   // controllers, rumble, and the bindings file

        // Read every frame: an editor switching projects can change the tick rate.
        const double dt = 1.0 / m_config.tickRate;
        while (accumulator >= dt) {
            SW_PROFILE_SCOPE("Fixed tick");
            app.onUpdate(dt);
            accumulator -= dt;
        }
        m_interpolationAlpha = std::clamp(accumulator / dt, 0.0, 1.0);
        {
            SW_PROFILE_SCOPE("Application frame");
            app.onFrame(frameTime);
        }

        if (m_renderer->beginFrame()) {
            if (m_config.gui) {
                pollGuiTheme(frameTime);
                m_renderer->newGuiFrame();
                ImGui_ImplSDL3_NewFrame();
                ImGui::NewFrame();
                app.onGui();
                ImGui::Render();
            }
            {
                SW_PROFILE_SCOPE("Gather draws");
                app.onRender(*m_renderer, m_interpolationAlpha);
            }
            {
                SW_PROFILE_SCOPE("Record and submit");
                m_renderer->endFrame();
            }
            if (m_config.gui) {
                SW_PROFILE_SCOPE("Detached GUI windows");
                m_renderer->renderGuiWindows();
            }
        } else {
            SDL_Delay(10);   // minimized: don't spin the CPU
        }

        const Uint64 frameEnd = SDL_GetPerformanceCounter();
        m_frameStats.add(
            {static_cast<float>(static_cast<double>(frameEnd - frameStart) * 1000.0 / freq), m_renderer->gpuFrameMs(),
             static_cast<float>(rawFrameTime * 1000.0), presentMs});

        fpsTimer += rawFrameTime;
        ++fpsFrames;
        if (fpsTimer >= 0.5) {
            updatePacingPeriod();   // the window may have moved to another display
            m_fps = fpsFrames / fpsTimer;
            if (m_config.showFpsInTitle) {
                char title[256];
                std::snprintf(
                    title, sizeof(title), "%s | %.0f fps (%.2f ms) | %ux%u | %u draws", m_config.window.title.c_str(),
                    m_fps, 1000.0 / m_fps, m_renderer->renderWidth(), m_renderer->renderHeight(),
                    m_renderer->drawCount());
                m_window->setTitle(title);
            }
            fpsTimer = 0.0;
            fpsFrames = 0;
        }

        if (m_config.maxFrames > 0 && ++frames >= m_config.maxFrames) {
            m_running = false;
        }
        SW_PROFILE_FRAME();
    }

    m_renderer->waitIdle();
    app.onShutdown();
}

}   // namespace shockwave
