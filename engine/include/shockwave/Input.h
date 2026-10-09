#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

union SDL_Event;

namespace shockwave {

class Window;

// Physical key positions (USB HID usage codes), so WASD stays WASD on any keyboard layout.
enum class Key : uint16_t {
    Unknown = 0,
    A = 4, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num1 = 30, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9, Num0,
    Enter = 40, Escape = 41, Backspace = 42, Tab = 43, Space = 44,
    F1 = 58, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Right = 79, Left = 80, Down = 81, Up = 82,
    LeftCtrl = 224, LeftShift = 225, LeftAlt = 226,
    RightCtrl = 228, RightShift = 229, RightAlt = 230,
};

enum class MouseButton : uint8_t { Left = 1, Middle = 2, Right = 3, X1 = 4, X2 = 5 };

// Keyboard and mouse state, plus named actions that games bind keys and buttons to.
// Game code asks "is move_forward held?" rather than "is W held?", so controls can be
// remapped (and gamepads added) without touching gameplay.
//
// Threading: the engine updates input on the main thread before the frame's hooks run.
// Reading it from any thread during the frame is safe; binding actions is main-thread only.
class Input {
public:
    explicit Input(Window& window);

    // Raw state. "Pressed" and "released" are true only on the frame the change happened.
    bool isKeyDown(Key key) const;
    bool wasKeyPressed(Key key) const;
    bool wasKeyReleased(Key key) const;
    bool isMouseDown(MouseButton button) const;
    bool wasMousePressed(MouseButton button) const;
    bool wasMouseReleased(MouseButton button) const;
    glm::vec2 mouseDelta() const { return m_mouseDelta; }    // pixels moved this frame
    glm::vec2 mousePosition() const { return m_mousePosition; }
    float scrollDelta() const { return m_scrollDelta; }

    // Actions. An action is held if any of its bindings is held.
    void bindAction(const std::string& action, Key key);
    void bindAction(const std::string& action, MouseButton button);
    void clearAction(const std::string& action);
    bool isActionDown(const std::string& action) const;
    bool wasActionPressed(const std::string& action) const;
    bool wasActionReleased(const std::string& action) const;
    // -1, 0 or +1 from a pair of actions, e.g. axis("move_left", "move_right").
    float axis(const std::string& negative, const std::string& positive) const;

    // Captured: cursor hidden and locked to the window, mouseDelta keeps working at the edges.
    void setMouseCaptured(bool captured);
    bool isMouseCaptured() const { return m_mouseCaptured; }

    // Called by the engine.
    void beginFrame();
    void handleEvent(const SDL_Event& event);

private:
    struct Binding {
        bool isMouse = false;
        uint16_t code = 0;
    };
    enum Flags : uint8_t { Down = 1, Pressed = 2, Released = 4 };

    uint8_t bindingState(const Binding& binding) const;
    uint8_t actionState(const std::string& action) const;

    Window& m_window;
    std::array<uint8_t, 512> m_keys{};
    std::array<uint8_t, 8> m_mouse{};
    glm::vec2 m_mouseDelta{0.0f};
    glm::vec2 m_mousePosition{0.0f};
    float m_scrollDelta = 0.0f;
    bool m_mouseCaptured = false;
    std::unordered_map<std::string, std::vector<Binding>> m_actions;
};

} // namespace shockwave
