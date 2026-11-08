#pragma once

#include <string>
#include <vector>

struct SDL_Window;

namespace shockwave {

struct WindowDesc {
    std::string title = "Shockwave";
    int width = 1280;
    int height = 720;
    bool vsync = false;   // off = render as fast as the GPU allows (mailbox, no tearing where supported)
    bool hidden = false;   // never shown; for command-line tools that need the renderer
    bool vulkan = true;   // off: no Vulkan driver needed (null renderer); ignored on macOS
};

// How the window covers the screen (S08, docs/Preferences.md#display).
enum class WindowMode {
    Windowed,   // a window of its own size
    Borderless,   // the whole display at the desktop's resolution, without changing it
    Fullscreen,   // exclusive: the display switches to the chosen resolution
};
const char* windowModeName(WindowMode mode);   // "windowed", "borderless", "fullscreen"

// A resolution the display offers for exclusive fullscreen.
struct DisplayResolution {
    int width = 0, height = 0;
    float refreshRate = 0;
    bool operator==(const DisplayResolution&) const = default;
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

    // ---- Display mode (platform calls only; renderers follow the new size) ----
    WindowMode mode() const;
    // The window's size in points, as a windowed size is given.
    int width() const;
    int height() const;
    // Switches mode, and for Windowed and Fullscreen the size (0 keeps the current one).
    // False with why when the platform refuses; the window is left as it was then.
    bool setMode(WindowMode mode, int width, int height, std::string& error);
    // What exclusive fullscreen can use on the window's display, largest first; empty
    // when the display offers none (offscreen and some virtual displays).
    std::vector<DisplayResolution> resolutions() const;
    bool hasFocus() const;

    SDL_Window* handle() const { return m_window; }

private:
    SDL_Window* m_window = nullptr;
};

}   // namespace shockwave
