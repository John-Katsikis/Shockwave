#include <Platform/Window.h>

#include <Core/Plugin.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace shockwave {

namespace {

// macOS has no system Vulkan loader, and the dynamic linker no longer searches
// /usr/local/lib, so SDL won't find the Vulkan SDK's loader by name. Point it at one:
// next to the executable first (a packaged game ships its own), then the Vulkan SDK and
// Homebrew. SDL_VULKAN_LIBRARY, when set, still wins.
void findVulkanLoader() {
#if defined(__APPLE__)
    if (SDL_GetHint(SDL_HINT_VULKAN_LIBRARY)) return;
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates = {executableDir() / "libvulkan.1.dylib", executableDir() / "libMoltenVK.dylib"};
    if (const char* sdk = std::getenv("VULKAN_SDK"); sdk && *sdk)
        candidates.push_back(fs::path(sdk) / "lib" / "libvulkan.1.dylib");
    candidates.emplace_back("/usr/local/lib/libvulkan.1.dylib");
    candidates.emplace_back("/opt/homebrew/lib/libvulkan.1.dylib");
    std::error_code ec;
    for (const fs::path& path : candidates) {
        if (fs::is_regular_file(path, ec)) {
            SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, path.string().c_str());
            return;
        }
    }
#endif
}

}   // namespace

Window::Window(const WindowDesc& desc) {
    findVulkanLoader();
#ifdef __APPLE__
    // SDL on macOS makes a window with no graphics flag an OpenGL one, which CI can't load;
    // MoltenVK always loads, so keep Vulkan there.
    const bool vulkan = true;
#else
    const bool vulkan = desc.vulkan;
#endif
    m_window = SDL_CreateWindow(
        desc.title.c_str(), desc.width, desc.height,
        (vulkan ? SDL_WINDOW_VULKAN : 0) | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
            (desc.hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!m_window) {
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }
}

Window::~Window() {
    if (m_window) SDL_DestroyWindow(m_window);
}

int Window::pixelWidth() const {
    int w = 0;
    SDL_GetWindowSizeInPixels(m_window, &w, nullptr);
    return w;
}

int Window::pixelHeight() const {
    int h = 0;
    SDL_GetWindowSizeInPixels(m_window, nullptr, &h);
    return h;
}

bool Window::isMinimized() const {
    return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) != 0 || pixelWidth() == 0 || pixelHeight() == 0;
}

void Window::setTitle(const std::string& title) { SDL_SetWindowTitle(m_window, title.c_str()); }

}   // namespace shockwave

namespace shockwave {

const char* windowModeName(WindowMode mode) {
    switch (mode) {
    case WindowMode::Windowed: return "windowed";
    case WindowMode::Borderless: return "borderless";
    case WindowMode::Fullscreen: return "fullscreen";
    }
    return "windowed";
}

WindowMode Window::mode() const {
    if (!(SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN)) return WindowMode::Windowed;
    return SDL_GetWindowFullscreenMode(m_window) ? WindowMode::Fullscreen : WindowMode::Borderless;
}

int Window::width() const {
    int w = 0;
    SDL_GetWindowSize(m_window, &w, nullptr);
    return w;
}

int Window::height() const {
    int h = 0;
    SDL_GetWindowSize(m_window, nullptr, &h);
    return h;
}

bool Window::setMode(WindowMode mode, int width, int height, std::string& error) {
    error.clear();
    if (width < 0 || height < 0 || width > 16384 || height > 16384) {
        error = "the size must be from 1 to 16384 pixels each way";
        return false;
    }
    switch (mode) {
    case WindowMode::Windowed:
        if (!SDL_SetWindowFullscreen(m_window, false)) {
            error = std::string("couldn't leave fullscreen: ") + SDL_GetError();
            return false;
        }
        if (width > 0 && height > 0 && !SDL_SetWindowSize(m_window, width, height)) {
            error = std::string("couldn't resize the window: ") + SDL_GetError();
            return false;
        }
        break;
    case WindowMode::Borderless:
        if (!SDL_SetWindowFullscreenMode(m_window, nullptr) || !SDL_SetWindowFullscreen(m_window, true)) {
            error = std::string("couldn't go borderless: ") + SDL_GetError();
            return false;
        }
        break;
    case WindowMode::Fullscreen: {
        const SDL_DisplayID display = SDL_GetDisplayForWindow(m_window);
        SDL_DisplayMode closest{};
        const int w = width > 0 ? width : this->width(), h = height > 0 ? height : this->height();
        if (!display || !SDL_GetClosestFullscreenDisplayMode(display, w, h, 0.0f, true, &closest)) {
            error = "the display offers no " + std::to_string(w) + " x " + std::to_string(h) + " fullscreen mode";
            return false;
        }
        if (!SDL_SetWindowFullscreenMode(m_window, &closest) || !SDL_SetWindowFullscreen(m_window, true)) {
            error = std::string("couldn't go fullscreen: ") + SDL_GetError();
            return false;
        }
        break;
    }
    }
    SDL_SyncWindow(m_window);
    return true;
}

std::vector<DisplayResolution> Window::resolutions() const {
    std::vector<DisplayResolution> out;
    const SDL_DisplayID display = SDL_GetDisplayForWindow(m_window);
    if (!display) return out;
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count);
    if (!modes) return out;
    for (int i = 0; i < count; ++i) {
        const DisplayResolution r{modes[i]->w, modes[i]->h, modes[i]->refresh_rate};
        if (std::find_if(out.begin(), out.end(), [&](const DisplayResolution& o) {
                return o.width == r.width && o.height == r.height;
            }) == out.end())
            out.push_back(r);
    }
    SDL_free(modes);
    return out;
}

bool Window::hasFocus() const { return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_INPUT_FOCUS) != 0; }

}   // namespace shockwave
