#include <Input/Input.h>
#include <Platform/Window.h>

#include <SDL3/SDL.h>

namespace shockwave {

Input::Input(Window& window) : m_window(window) {}

bool Input::isKeyDown(Key key) const { return m_keys[static_cast<size_t>(key)] & Down; }
bool Input::wasKeyPressed(Key key) const { return m_keys[static_cast<size_t>(key)] & Pressed; }
bool Input::wasKeyReleased(Key key) const { return m_keys[static_cast<size_t>(key)] & Released; }
bool Input::isMouseDown(MouseButton b) const { return m_mouse[static_cast<size_t>(b)] & Down; }
bool Input::wasMousePressed(MouseButton b) const { return m_mouse[static_cast<size_t>(b)] & Pressed; }
bool Input::wasMouseReleased(MouseButton b) const { return m_mouse[static_cast<size_t>(b)] & Released; }

void Input::bindAction(const std::string& action, Key key) {
    m_actions[action].push_back({false, static_cast<uint16_t>(key)});
}

void Input::bindAction(const std::string& action, MouseButton button) {
    m_actions[action].push_back({true, static_cast<uint16_t>(button)});
}

void Input::clearAction(const std::string& action) {
    m_actions.erase(action);
}

uint8_t Input::bindingState(const Binding& binding) const {
    if (binding.isMouse) return binding.code < m_mouse.size() ? m_mouse[binding.code] : 0;
    return binding.code < m_keys.size() ? m_keys[binding.code] : 0;
}

uint8_t Input::actionState(const std::string& action) const {
    auto it = m_actions.find(action);
    if (it == m_actions.end()) return 0;
    uint8_t state = 0;
    for (const Binding& b : it->second) state |= bindingState(b);
    return state;
}

bool Input::isActionDown(const std::string& action) const { return actionState(action) & Down; }
bool Input::wasActionPressed(const std::string& action) const { return actionState(action) & Pressed; }
bool Input::wasActionReleased(const std::string& action) const { return actionState(action) & Released; }

float Input::axis(const std::string& negative, const std::string& positive) const {
    return (isActionDown(positive) ? 1.0f : 0.0f) - (isActionDown(negative) ? 1.0f : 0.0f);
}

void Input::setMouseCaptured(bool captured) {
    if (captured == m_mouseCaptured) return;
    SDL_SetWindowRelativeMouseMode(m_window.handle(), captured);
    m_mouseCaptured = captured;
}

void Input::beginFrame() {
    for (uint8_t& k : m_keys) k &= Down;
    for (uint8_t& b : m_mouse) b &= Down;
    m_mouseDelta = glm::vec2(0.0f);
    m_scrollDelta = 0.0f;
}

void Input::handleEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        if (!event.key.repeat && event.key.scancode < m_keys.size()) {
            m_keys[event.key.scancode] |= Down | Pressed;
        }
        break;
    case SDL_EVENT_KEY_UP:
        if (event.key.scancode < m_keys.size()) {
            m_keys[event.key.scancode] = static_cast<uint8_t>((m_keys[event.key.scancode] & ~Down) | Released);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.button < m_mouse.size()) m_mouse[event.button.button] |= Down | Pressed;
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button < m_mouse.size()) {
            m_mouse[event.button.button] = static_cast<uint8_t>((m_mouse[event.button.button] & ~Down) | Released);
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        m_mouseDelta += glm::vec2(event.motion.xrel, event.motion.yrel);
        m_mousePosition = glm::vec2(event.motion.x, event.motion.y);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        m_scrollDelta += event.wheel.y;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        // Don't leave keys stuck down when the window loses focus mid-press.
        for (uint8_t& k : m_keys) k = (k & Down) ? Released : 0;
        for (uint8_t& b : m_mouse) b = (b & Down) ? Released : 0;
        setMouseCaptured(false);
        break;
    default:
        break;
    }
}

} // namespace shockwave
