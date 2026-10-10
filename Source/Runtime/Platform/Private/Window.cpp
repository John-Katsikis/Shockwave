#include <Platform/Window.h>

#include <SDL3/SDL.h>

#include <stdexcept>

namespace shockwave {

Window::Window(const WindowDesc& desc) {
    m_window = SDL_CreateWindow(desc.title.c_str(), desc.width, desc.height,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
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
    return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) != 0
        || pixelWidth() == 0 || pixelHeight() == 0;
}

void Window::setTitle(const std::string& title) {
    SDL_SetWindowTitle(m_window, title.c_str());
}

} // namespace shockwave
