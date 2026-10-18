#include <Platform/Window.h>

#include <Core/Plugin.h>

#include <SDL3/SDL.h>

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
