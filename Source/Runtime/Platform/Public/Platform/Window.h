#pragma once

#include <string>

struct SDL_Window;

namespace shockwave {

struct WindowDesc {
    std::string title = "Shockwave";
    int width = 1280;
    int height = 720;
    bool vsync = false;   // off = render as fast as the GPU allows (mailbox, no tearing where supported)
    bool hidden = false;   // never shown; for command-line tools that need the renderer
    bool vulkan = true;   // off: no Vulkan surface, so no Vulkan driver is needed (null renderer)
};

// Owns the OS window. The renderer creates its Vulkan surface from it.
class Window {
public:
    explicit Window(const WindowDesc& desc);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Size of the drawable area in pixels (can differ from window size on HiDPI).
    int pixelWidth() const;
    int pixelHeight() const;
    bool isMinimized() const;
    void setTitle(const std::string& title);

    SDL_Window* handle() const { return m_window; }

private:
    SDL_Window* m_window = nullptr;
};

}   // namespace shockwave
