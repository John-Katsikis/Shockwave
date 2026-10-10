#include <Engine/Engine.h>

#include <UI/Gui.h>

#include <imgui_impl_sdl3.h>
#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace shockwave {

Engine::Engine(const EngineConfig& config) : m_config(config) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }
    m_jobs = std::make_unique<JobSystem>(m_config.workerThreads);
    m_window = std::make_unique<Window>(m_config.window);
    m_renderer = std::make_unique<Renderer>(*m_window, *m_jobs, m_config.window.vsync);
    m_input = std::make_unique<Input>(*m_window);
    m_assets = std::make_unique<Assets>();
    if (m_config.gui) initGui();
    std::printf(
        "Shockwave: rendering with Vulkan on %s, %u worker threads\n", m_renderer->gpuName(), m_jobs->workerCount());
}

Engine::~Engine() {
    if (m_config.gui) shutdownGui();
    m_input.reset();
    m_renderer.reset();
    m_window.reset();
    m_jobs.reset();
    SDL_Quit();
}

void Engine::initGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;

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

    m_dpiScale = SDL_GetWindowDisplayScale(m_window->handle());
    if (m_dpiScale <= 0.0f) m_dpiScale = 1.0f;
    loadGuiTheme(false);

    ImGui_ImplSDL3_InitForVulkan(m_window->handle());
    m_renderer->initGui();
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
            std::fprintf(stderr, "Shockwave: UI theme not applied: %s\n", error.c_str());
            if (reload) return;
            theme = gui::Theme();
        } else if (reload) {
            std::printf("Shockwave: reloaded UI theme %s\n", m_themePath.c_str());
        }
    } else if (!m_config.guiTheme.empty()) {
        std::fprintf(stderr, "Shockwave: %s not found, using the built-in UI theme\n", m_config.guiTheme.c_str());
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

void Engine::run(Game& game) {
    // Fixed-timestep loop: simulation ticks at tickRate, rendering runs as fast as vsync allows.
    const double maxFrameTime = 0.25;   // avoid the "spiral of death" after a long stall
    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());

    double accumulator = 0.0;
    Uint64 previous = SDL_GetPerformanceCounter();
    int frames = 0;

    double fpsTimer = 0.0;
    int fpsFrames = 0;

    m_running = true;
    game.m_engine = this;
    game.onStart();

    while (m_running) {
        m_input->beginFrame();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (m_config.gui) ImGui_ImplSDL3_ProcessEvent(&event);
            m_input->handleEvent(event);
            switch (event.type) {
            case SDL_EVENT_QUIT:
                if (game.onQuitRequested()) m_running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (m_config.quitOnEscape && event.key.scancode == SDL_SCANCODE_ESCAPE) m_running = false;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: m_renderer->onResize(); break;
            default: break;
            }
        }

        const Uint64 now = SDL_GetPerformanceCounter();
        double frameTime = static_cast<double>(now - previous) / freq;
        previous = now;
        if (frameTime > maxFrameTime) frameTime = maxFrameTime;
        accumulator += frameTime;

        // Read every frame: an editor switching projects can change the tick rate.
        const double dt = 1.0 / m_config.tickRate;
        while (accumulator >= dt) {
            game.onUpdate(dt);
            accumulator -= dt;
        }
        game.onFrame(frameTime);

        if (m_renderer->beginFrame()) {
            if (m_config.gui) {
                pollGuiTheme(frameTime);
                m_renderer->newGuiFrame();
                ImGui_ImplSDL3_NewFrame();
                ImGui::NewFrame();
                game.onGui();
                ImGui::Render();
            }
            game.onRender(*m_renderer, accumulator / dt);
            m_renderer->endFrame();
        } else {
            SDL_Delay(10);   // minimized: don't spin the CPU
        }

        fpsTimer += frameTime;
        ++fpsFrames;
        if (fpsTimer >= 0.5) {
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
    }

    m_renderer->waitIdle();
    game.onShutdown();
}

}   // namespace shockwave
