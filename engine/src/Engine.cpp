#include <shockwave/Engine.h>
#include <shockwave/Gui.h>

#include <SDL3/SDL.h>
#include <imgui_impl_sdl3.h>

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
    if (m_config.gui) initGui();
    std::printf("Shockwave: rendering with Vulkan on %s, %u worker threads\n",
                m_renderer->gpuName(), m_jobs->workerCount());
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

    float dpiScale = SDL_GetWindowDisplayScale(m_window->handle());
    if (dpiScale <= 0.0f) dpiScale = 1.0f;
    gui::applyTheme(dpiScale);

    const std::string fontPath = std::string(SDL_GetBasePath()) + "fonts/Roboto-Medium.ttf";
    if (SDL_GetPathInfo(fontPath.c_str(), nullptr)) {
        io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 15.0f);
    } else {
        io.Fonts->AddFontDefault();
    }

    ImGui_ImplSDL3_InitForVulkan(m_window->handle());
    m_renderer->initGui();
}

void Engine::shutdownGui() {
    m_renderer->shutdownGui();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void Engine::run(Game& game) {
    // Fixed-timestep loop: simulation ticks at tickRate, rendering runs as fast as vsync allows.
    const double dt = 1.0 / m_config.tickRate;
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
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                m_renderer->onResize();
                break;
            default:
                break;
            }
        }

        const Uint64 now = SDL_GetPerformanceCounter();
        double frameTime = static_cast<double>(now - previous) / freq;
        previous = now;
        if (frameTime > maxFrameTime) frameTime = maxFrameTime;
        accumulator += frameTime;

        while (accumulator >= dt) {
            game.onUpdate(dt);
            accumulator -= dt;
        }
        game.onFrame(frameTime);

        if (m_renderer->beginFrame()) {
            if (m_config.gui) {
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
                std::snprintf(title, sizeof(title), "%s | %.0f fps (%.2f ms) | %ux%u | %u draws",
                              m_config.window.title.c_str(), m_fps, 1000.0 / m_fps, m_renderer->renderWidth(),
                              m_renderer->renderHeight(), m_renderer->drawCount());
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

} // namespace shockwave
