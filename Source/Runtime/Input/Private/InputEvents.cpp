// Raw input events for UI (Input::events): keys with repeat and modifiers, typed text,
// and mouse buttons and wheel with positions, in the order they happened.
#include <Input/Input.h>
#include <Platform/Window.h>

#include <SDL3/SDL.h>

namespace shockwave {

namespace {

uint16_t modifiers(SDL_Keymod mod) {
    uint16_t out = 0;
    if (mod & SDL_KMOD_SHIFT) out |= InputEvent::Shift;
    if (mod & SDL_KMOD_CTRL) out |= InputEvent::Ctrl;
    if (mod & SDL_KMOD_ALT) out |= InputEvent::Alt;
    if (mod & SDL_KMOD_GUI) out |= InputEvent::Super;
    if (mod & SDL_KMOD_CAPS) out |= InputEvent::CapsLock;
    if (mod & SDL_KMOD_NUM) out |= InputEvent::NumLock;
    return out;
}

}   // namespace

void Input::recordEvent(const SDL_Event& event) {
    InputEvent e;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        e.type = event.type == SDL_EVENT_KEY_DOWN ? InputEvent::Type::KeyDown : InputEvent::Type::KeyUp;
        e.repeat = event.key.repeat;
        e.code = static_cast<uint16_t>(event.key.scancode);
        e.modifiers = modifiers(event.key.mod);
        e.window = event.key.windowID;
        break;
    case SDL_EVENT_TEXT_INPUT: {
        if (!event.text.text) return;
        const std::string_view text(event.text.text);
        e.type = InputEvent::Type::Text;
        e.window = event.text.windowID;
        e.textOffset = static_cast<uint32_t>(m_eventText.size());
        e.textSize = static_cast<uint32_t>(text.size());
        m_eventText += text;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        e.type = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? InputEvent::Type::MouseDown : InputEvent::Type::MouseUp;
        e.code = event.button.button;
        e.modifiers = modifiers(SDL_GetModState());
        e.window = event.button.windowID;
        e.position = {event.button.x, event.button.y};
        break;
    case SDL_EVENT_MOUSE_MOTION:
        e.type = InputEvent::Type::MouseMove;
        e.modifiers = modifiers(SDL_GetModState());
        e.window = event.motion.windowID;
        e.position = {event.motion.x, event.motion.y};
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        e.type = InputEvent::Type::Wheel;
        e.modifiers = modifiers(SDL_GetModState());
        e.window = event.wheel.windowID;
        e.position = {event.wheel.mouse_x, event.wheel.mouse_y};
        e.wheel = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
        break;
    default: return;
    }
    m_events.push_back(e);
}

void Input::setTextInput(bool enabled) {
    if (enabled == m_textInput) return;
    m_textInput = enabled;
    if (!m_window) return;
    if (enabled)
        SDL_StartTextInput(m_window->handle());
    else
        SDL_StopTextInput(m_window->handle());
}

}   // namespace shockwave
