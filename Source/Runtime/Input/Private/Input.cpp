#include <Input/Input.h>

#include <Platform/Window.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace shockwave {

Input::Input(Window* window) : m_window(window) {
    // Gamepads and joysticks are optional: without the subsystem, input still works.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    // Devices already plugged in arrive as ADDED events on the first poll.
}

Input::~Input() {
    for (Handles& h : m_handles) {
        if (h.pad)
            SDL_CloseGamepad(h.pad);
        else if (h.joystick)
            SDL_CloseJoystick(h.joystick);
    }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

bool Input::isKeyDown(Key key) const { return m_keys[static_cast<size_t>(key)] & Down; }
bool Input::wasKeyPressed(Key key) const { return m_keys[static_cast<size_t>(key)] & Pressed; }
bool Input::wasKeyReleased(Key key) const { return m_keys[static_cast<size_t>(key)] & Released; }
bool Input::isMouseDown(MouseButton b) const { return m_mouse[static_cast<size_t>(b)] & Down; }
bool Input::wasMousePressed(MouseButton b) const { return m_mouse[static_cast<size_t>(b)] & Pressed; }
bool Input::wasMouseReleased(MouseButton b) const { return m_mouse[static_cast<size_t>(b)] & Released; }

// ---- Bindings ----------------------------------------------------------------------------

void Input::bindAction(const std::string& action, Key key) {
    bindAction(action, InputBinding{.source = InputBinding::Source::Key, .code = static_cast<uint16_t>(key)});
}

void Input::bindAction(const std::string& action, MouseButton button) {
    bindAction(action, InputBinding{.source = InputBinding::Source::Mouse, .code = static_cast<uint16_t>(button)});
}

void Input::bindAction(const std::string& action, GamepadButton button) {
    bindAction(
        action, InputBinding{.source = InputBinding::Source::GamepadButton, .code = static_cast<uint16_t>(button)});
}

void Input::bindAction(const std::string& action, GamepadAxis axis, InputBinding::Range range) {
    bindAction(
        action,
        InputBinding{.source = InputBinding::Source::GamepadAxis, .code = static_cast<uint16_t>(axis), .range = range});
}

void Input::bindAction(const std::string& action, const InputBinding& binding) {
    m_builtIn[action].push_back(binding);
    rebuildActions();
}

void Input::clearAction(const std::string& action) {
    m_builtIn.erase(action);
    rebuildActions();
}

void Input::rebuildActions() {
    m_actions = m_builtIn;
    m_deadzone = m_hasFile ? m_file.deadzone : 0.15f;
    if (m_hasFile) {
        std::map<std::string, std::vector<InputBinding>> fromFile;
        for (const auto& [action, binding] : m_file.binds) fromFile[action].push_back(binding);
        for (auto& [action, bindings] : fromFile) m_actions[action] = std::move(bindings);
    }
    ++m_bindingsVersion;
}

InputBindings Input::currentBindings() const {
    InputBindings out;
    out.deadzone = m_deadzone;
    for (const auto& [action, bindings] : m_actions)
        for (const InputBinding& b : bindings) out.binds.emplace_back(action, b);
    return out;
}

bool Input::loadBindingsFile(const std::filesystem::path& path, std::string& error) {
    InputBindings loaded;
    m_bindingsPath = path;
    std::error_code ec;
    m_bindingsTime = std::filesystem::last_write_time(path, ec);
    if (!loadInputBindings(path, loaded, error)) {
        m_bindingsError = error;
        return false;
    }
    m_bindingsError.clear();
    setBindings(loaded);
    return true;
}

void Input::setBindings(const InputBindings& bindings) {
    m_file = bindings;
    m_hasFile = true;
    rebuildActions();
}

void Input::clearBindingsFile() {
    m_bindingsPath.clear();
    m_bindingsError.clear();
    m_file = {};
    m_hasFile = false;
    rebuildActions();
}

void Input::checkBindingsFile(double frameDt) {
    if (m_bindingsPath.empty()) return;
    m_bindingsCheckTimer -= frameDt;
    if (m_bindingsCheckTimer > 0.0) return;
    m_bindingsCheckTimer = 0.5;
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(m_bindingsPath, ec);
    if (ec || time == m_bindingsTime) return;
    std::string error;
    loadBindingsFile(m_bindingsPath, error);   // keeps the old bindings if it fails
}

// ---- Actions -----------------------------------------------------------------------------

namespace {

float shapeAxis(float raw, const InputBinding& b, float deadzone) {
    if (b.invert) raw = -raw;
    switch (b.range) {
    case InputBinding::Range::Full: return std::clamp((raw + 1.0f) * 0.5f, 0.0f, 1.0f);
    case InputBinding::Range::Negative: raw = -raw; break;
    case InputBinding::Range::Positive: break;
    }
    if (raw <= deadzone) return 0.0f;
    return std::min(1.0f, (raw - deadzone) / (1.0f - deadzone));
}

bool matchesDevice(const std::string& filter, const std::string& name) {
    if (filter == "*") return true;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    return lower(name).find(lower(filter)) != std::string::npos;
}

}   // namespace

float Input::bindingValue(const InputBinding& b, bool previous, uint8_t sources) const {
    using S = InputBinding::Source;
    auto flagValue = [&](uint8_t flags) {
        // The previous frame's state, from this frame's flags.
        const bool down = flags & Down;
        if (!previous) return down ? 1.0f : 0.0f;
        const bool was = (down && !(flags & Pressed)) || (!down && (flags & Released));
        return was ? 1.0f : 0.0f;
    };
    float best = 0.0f;
    switch (b.source) {
    case S::Key:
        if (!(sources & KeyboardMouse)) return 0.0f;
        return b.code < m_keys.size() ? flagValue(m_keys[b.code]) : 0.0f;
    case S::Mouse:
        if (!(sources & KeyboardMouse)) return 0.0f;
        return b.code < m_mouse.size() ? flagValue(m_mouse[b.code]) : 0.0f;
    case S::GamepadButton:
    case S::GamepadAxis:
        if (!(sources & Gamepads)) return 0.0f;
        for (size_t i = 0; i < m_devices.size(); ++i) {
            if (m_devices[i].kind != InputDevice::Kind::Gamepad) continue;
            const Handles& h = m_handles[i];
            if (b.source == S::GamepadButton) {
                if (b.code < h.buttonFlags.size()) best = std::max(best, flagValue(h.buttonFlags[b.code]));
            } else if (b.code < m_devices[i].axes.size()) {
                const float raw = previous ? h.previousAxes[b.code] : m_devices[i].axes[b.code];
                best = std::max(best, shapeAxis(raw, b, b.code >= 4 ? 0.05f : m_deadzone));
            }
        }
        return best;
    case S::JoystickButton:
    case S::JoystickAxis:
    case S::JoystickHat:
        if (!(sources & Joysticks)) return 0.0f;
        for (size_t i = 0; i < m_devices.size(); ++i) {
            const InputDevice& d = m_devices[i];
            if (d.kind != InputDevice::Kind::Joystick || !matchesDevice(b.device, d.name)) continue;
            const Handles& h = m_handles[i];
            if (b.source == S::JoystickButton) {
                if (b.code < h.buttonFlags.size()) best = std::max(best, flagValue(h.buttonFlags[b.code]));
            } else if (b.source == S::JoystickAxis) {
                if (b.code < d.axes.size()) {
                    const float raw = previous ? h.previousAxes[b.code] : d.axes[b.code];
                    best = std::max(best, shapeAxis(raw, b, m_deadzone));
                }
            } else if (const size_t hat = b.code / 4u; hat < d.hats.size()) {
                const uint8_t bits = previous ? h.previousHats[hat] : d.hats[hat];
                if (bits & (1u << (b.code % 4u))) best = 1.0f;
            }
        }
        return best;
    }
    return 0.0f;
}

uint8_t Input::actionState(const std::string& action, uint8_t sources) const {
    auto it = m_actions.find(action);
    if (it == m_actions.end()) return 0;
    float now = 0.0f, before = 0.0f;
    for (const InputBinding& b : it->second) {
        now = std::max(now, bindingValue(b, false, sources));
        before = std::max(before, bindingValue(b, true, sources));
    }
    // A tap that went down and up within one frame still counts as a press.
    bool tapped = false;
    for (const InputBinding& b : it->second) {
        uint8_t flags = 0;
        if (b.source == InputBinding::Source::Key && (sources & KeyboardMouse) && b.code < m_keys.size())
            flags = m_keys[b.code];
        else if (b.source == InputBinding::Source::Mouse && (sources & KeyboardMouse) && b.code < m_mouse.size())
            flags = m_mouse[b.code];
        if ((flags & Pressed) && (flags & Released)) tapped = true;
    }
    uint8_t state = 0;
    const bool down = now >= 0.5f, wasDown = before >= 0.5f;
    if (down) state |= Down;
    if ((down && !wasDown) || tapped) state |= Pressed;
    if ((!down && wasDown) || tapped) state |= Released;
    return state;
}

bool Input::isActionDown(const std::string& a, uint8_t s) const { return actionState(a, s) & Down; }
bool Input::wasActionPressed(const std::string& a, uint8_t s) const { return actionState(a, s) & Pressed; }
bool Input::wasActionReleased(const std::string& a, uint8_t s) const { return actionState(a, s) & Released; }

float Input::actionValue(const std::string& action, uint8_t sources) const {
    auto it = m_actions.find(action);
    if (it == m_actions.end()) return 0.0f;
    float value = 0.0f;
    for (const InputBinding& b : it->second) value = std::max(value, bindingValue(b, false, sources));
    return value;
}

float Input::axis(const std::string& negative, const std::string& positive, uint8_t sources) const {
    return actionValue(positive, sources) - actionValue(negative, sources);
}

void Input::setMouseCaptured(bool captured) {
    if (captured == m_mouseCaptured || !m_window) return;
    if (captured) {
        // The window under the mouse: an editor panel may live in a window of its own.
        m_capturedWindow = SDL_GetMouseFocus();
        if (!m_capturedWindow) m_capturedWindow = m_window->handle();
    }
    if (m_capturedWindow) SDL_SetWindowRelativeMouseMode(m_capturedWindow, captured);
    if (!captured) m_capturedWindow = nullptr;
    m_mouseCaptured = captured;
}

// ---- Frame -------------------------------------------------------------------------------

void Input::beginFrame() {
    for (uint8_t& k : m_keys) k &= Down;
    for (uint8_t& b : m_mouse) b &= Down;
    for (Handles& h : m_handles)
        for (uint8_t& b : h.buttonFlags) b &= Down;
    for (size_t i = 0; i < m_devices.size(); ++i) {
        m_handles[i].previousAxes = m_devices[i].axes;
        m_handles[i].previousHats = m_devices[i].hats;
    }
    m_mouseDelta = glm::vec2(0.0f);
    m_scrollDelta = 0.0f;
    m_lastPressed.reset();
    m_events.clear();
    m_eventText.clear();
}

void Input::notePressed(const InputBinding& binding) {
    if (!m_lastPressed) m_lastPressed = binding;
}

void Input::handleEvent(const SDL_Event& event) {
    recordEvent(event);
    using S = InputBinding::Source;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        if (!event.key.repeat && event.key.scancode < m_keys.size()) {
            m_keys[event.key.scancode] |= Down | Pressed;
            if (!keyName(static_cast<uint16_t>(event.key.scancode)).empty())
                notePressed({.source = S::Key, .code = static_cast<uint16_t>(event.key.scancode)});
        }
        break;
    case SDL_EVENT_KEY_UP:
        if (event.key.scancode < m_keys.size()) {
            m_keys[event.key.scancode] = static_cast<uint8_t>((m_keys[event.key.scancode] & ~Down) | Released);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.button < m_mouse.size()) {
            m_mouse[event.button.button] |= Down | Pressed;
            notePressed({.source = S::Mouse, .code = event.button.button});
        }
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
    case SDL_EVENT_MOUSE_WHEEL: m_scrollDelta += event.wheel.y; break;

    // A gamepad is also a joystick: SDL sends both ADDED events, and openDevice takes
    // whichever comes first and opens it the right way.
    case SDL_EVENT_GAMEPAD_ADDED: openDevice(event.gdevice.which); break;
    case SDL_EVENT_JOYSTICK_ADDED: openDevice(event.jdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMOVED: closeDevice(event.gdevice.which); break;
    case SDL_EVENT_JOYSTICK_REMOVED: closeDevice(event.jdevice.which); break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const int i = deviceIndex(event.gbutton.which);
        if (i < 0 || m_devices[i].kind != InputDevice::Kind::Gamepad) break;
        // SDL's button order matches GamepadButton up to the touchpad; paddles are ignored.
        const int button = event.gbutton.button == SDL_GAMEPAD_BUTTON_TOUCHPAD
                               ? static_cast<int>(GamepadButton::Touchpad)
                           : event.gbutton.button == SDL_GAMEPAD_BUTTON_MISC1 ? static_cast<int>(GamepadButton::Misc)
                           : event.gbutton.button <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? event.gbutton.button
                                                                                   : -1;
        if (button < 0) break;
        uint8_t& flags = m_handles[i].buttonFlags[static_cast<size_t>(button)];
        if (event.gbutton.down) {
            flags |= Down | Pressed;
            notePressed({.source = S::GamepadButton, .code = static_cast<uint16_t>(button)});
        } else {
            flags = static_cast<uint8_t>((flags & ~Down) | Released);
        }
        m_devices[i].buttons[static_cast<size_t>(button)] = event.gbutton.down ? 1 : 0;
        break;
    }
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP: {
        const int i = deviceIndex(event.jbutton.which);
        if (i < 0 || m_devices[i].kind != InputDevice::Kind::Joystick) break;
        if (event.jbutton.button >= m_handles[i].buttonFlags.size()) break;
        uint8_t& flags = m_handles[i].buttonFlags[event.jbutton.button];
        if (event.jbutton.down) {
            flags |= Down | Pressed;
            notePressed({.source = S::JoystickButton, .code = event.jbutton.button, .device = m_devices[i].name});
        } else {
            flags = static_cast<uint8_t>((flags & ~Down) | Released);
        }
        m_devices[i].buttons[event.jbutton.button] = event.jbutton.down ? 1 : 0;
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        // Don't leave keys stuck down when the window loses focus mid-press.
        for (uint8_t& k : m_keys) k = (k & Down) ? Released : 0;
        for (uint8_t& b : m_mouse) b = (b & Down) ? Released : 0;
        setMouseCaptured(false);
        break;
    default: break;
    }
}

void Input::update(double frameDt) {
    pollDevices();
    applyFeedback();
    checkBindingsFile(frameDt);
}

// ---- Devices -----------------------------------------------------------------------------

int Input::deviceIndex(uint32_t id) const {
    for (size_t i = 0; i < m_devices.size(); ++i)
        if (m_devices[i].id == id) return static_cast<int>(i);
    return -1;
}

const InputDevice* Input::gamepad(int player) const {
    for (const InputDevice& d : m_devices)
        if (d.kind == InputDevice::Kind::Gamepad && d.player == player) return &d;
    return nullptr;
}

namespace {

GamepadType toGamepadType(SDL_GamepadType type) {
    switch (type) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE: return GamepadType::Xbox;
    case SDL_GAMEPAD_TYPE_PS3: return GamepadType::PlayStation3;
    case SDL_GAMEPAD_TYPE_PS4: return GamepadType::PlayStation4;
    case SDL_GAMEPAD_TYPE_PS5: return GamepadType::PlayStation5;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return GamepadType::Switch;
    case SDL_GAMEPAD_TYPE_STEAM: return GamepadType::Steam;
    case SDL_GAMEPAD_TYPE_UNKNOWN: return GamepadType::Unknown;
    default: return GamepadType::Other;
    }
}

float axisValue(Sint16 raw) { return std::clamp(static_cast<float>(raw) / 32767.0f, -1.0f, 1.0f); }

}   // namespace

void Input::openDevice(uint32_t id) {
    if (deviceIndex(id) >= 0) return;
    InputDevice d;
    Handles h;
    d.id = id;
    SDL_PropertiesID props = 0;
    if (SDL_IsGamepad(id)) {
        h.pad = SDL_OpenGamepad(id);
        if (!h.pad) return;
        h.joystick = SDL_GetGamepadJoystick(h.pad);
        d.kind = InputDevice::Kind::Gamepad;
        const char* name = SDL_GetGamepadName(h.pad);
        d.name = name ? name : "Gamepad";
        d.type = toGamepadType(SDL_GetGamepadType(h.pad));
        d.axes.assign(static_cast<size_t>(GamepadAxis::Count), 0.0f);
        d.buttons.assign(static_cast<size_t>(GamepadButton::Count), 0);
        // The lowest free player number; it also lights the pad's player LED.
        int player = 0;
        while (gamepad(player)) ++player;
        d.player = player;
        SDL_SetGamepadPlayerIndex(h.pad, player);
        if (SDL_GamepadHasSensor(h.pad, SDL_SENSOR_GYRO)) {
            SDL_SetGamepadSensorEnabled(h.pad, SDL_SENSOR_GYRO, true);
            SDL_SetGamepadSensorEnabled(h.pad, SDL_SENSOR_ACCEL, true);
            d.hasGyro = true;
        }
        props = SDL_GetGamepadProperties(h.pad);
    } else {
        h.joystick = SDL_OpenJoystick(id);
        if (!h.joystick) return;
        d.kind = InputDevice::Kind::Joystick;
        const char* name = SDL_GetJoystickName(h.joystick);
        d.name = name ? name : "Joystick";
        d.axes.assign(static_cast<size_t>(std::max(0, SDL_GetNumJoystickAxes(h.joystick))), 0.0f);
        d.buttons.assign(static_cast<size_t>(std::max(0, SDL_GetNumJoystickButtons(h.joystick))), 0);
        d.hats.assign(static_cast<size_t>(std::max(0, SDL_GetNumJoystickHats(h.joystick))), 0);
        for (size_t a = 0; a < d.axes.size(); ++a)
            d.axes[a] = axisValue(SDL_GetJoystickAxis(h.joystick, static_cast<int>(a)));
        props = SDL_GetJoystickProperties(h.joystick);
    }
    if (props) {
        d.hasRumble = SDL_GetBooleanProperty(props, SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false);
        d.hasTriggerRumble = SDL_GetBooleanProperty(props, SDL_PROP_JOYSTICK_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
        d.hasLight = SDL_GetBooleanProperty(props, SDL_PROP_JOYSTICK_CAP_RGB_LED_BOOLEAN, false);
    }
    h.buttonFlags.assign(d.buttons.size(), 0);
    h.previousAxes = d.axes;
    h.restAxes = d.axes;   // where pedals and throttles sit, for Listen
    h.previousHats = d.hats;
    m_devices.push_back(std::move(d));
    m_handles.push_back(std::move(h));
}

void Input::closeDevice(uint32_t id) {
    const int i = deviceIndex(id);
    if (i < 0) return;
    Handles& h = m_handles[static_cast<size_t>(i)];
    if (h.pad)
        SDL_CloseGamepad(h.pad);
    else if (h.joystick)
        SDL_CloseJoystick(h.joystick);
    m_devices.erase(m_devices.begin() + i);
    m_handles.erase(m_handles.begin() + i);
}

void Input::pollDevices() {
    using S = InputBinding::Source;
    for (size_t i = 0; i < m_devices.size(); ++i) {
        InputDevice& d = m_devices[i];
        Handles& h = m_handles[i];
        if (h.pad) {
            for (int a = 0; a < static_cast<int>(GamepadAxis::Count); ++a) {
                float v = axisValue(SDL_GetGamepadAxis(h.pad, static_cast<SDL_GamepadAxis>(a)));
                if (a == static_cast<int>(GamepadAxis::LeftY) || a == static_cast<int>(GamepadAxis::RightY)) v = -v;
                d.axes[static_cast<size_t>(a)] = v;
            }
            if (d.hasGyro) {
                float g[3] = {}, acc[3] = {};
                SDL_GetGamepadSensorData(h.pad, SDL_SENSOR_GYRO, g, 3);
                SDL_GetGamepadSensorData(h.pad, SDL_SENSOR_ACCEL, acc, 3);
                d.gyro = {g[0], g[1], g[2]};
                d.accel = {acc[0], acc[1], acc[2]};
            }
            // Listen: a stick or trigger pushed past halfway.
            for (size_t a = 0; a < d.axes.size(); ++a) {
                const float now = d.axes[a], before = h.previousAxes[a];
                if (std::abs(now) >= 0.5f && std::abs(before) < 0.5f) {
                    notePressed(
                        {.source = S::GamepadAxis,
                         .code = static_cast<uint16_t>(a),
                         .range = now > 0 ? InputBinding::Range::Positive : InputBinding::Range::Negative});
                }
            }
        } else {
            for (size_t a = 0; a < d.axes.size(); ++a) {
                d.axes[a] = axisValue(SDL_GetJoystickAxis(h.joystick, static_cast<int>(a)));
                // Listen: an axis moved halfway across its travel from where it rested.
                const float rest = h.restAxes[a];
                if (std::abs(d.axes[a] - rest) >= 1.0f && std::abs(h.previousAxes[a] - rest) < 1.0f) {
                    InputBinding b{.source = S::JoystickAxis, .code = static_cast<uint16_t>(a), .device = d.name};
                    if (std::abs(rest) > 0.7f) {   // a pedal or throttle resting at one end
                        b.range = InputBinding::Range::Full;
                        b.invert = rest > 0.0f;
                    } else {
                        b.range = d.axes[a] > rest ? InputBinding::Range::Positive : InputBinding::Range::Negative;
                    }
                    notePressed(b);
                }
            }
            for (size_t hat = 0; hat < d.hats.size(); ++hat) {
                d.hats[hat] = SDL_GetJoystickHat(h.joystick, static_cast<int>(hat));
                const uint8_t pressed = d.hats[hat] & ~h.previousHats[hat];
                for (uint16_t dir = 0; dir < 4; ++dir) {
                    if (pressed & (1u << dir))
                        notePressed(
                            {.source = S::JoystickHat, .code = static_cast<uint16_t>(hat * 4 + dir), .device = d.name});
                }
            }
        }
    }
}

void Input::rumble(float low, float high, double seconds, int player) {
    std::lock_guard lock(m_feedbackMutex);
    m_feedback.push_back({Feedback::Kind::Rumble, player, low, high, 0.0f, seconds});
}

void Input::rumbleTriggers(float left, float right, double seconds, int player) {
    std::lock_guard lock(m_feedbackMutex);
    m_feedback.push_back({Feedback::Kind::TriggerRumble, player, left, right, 0.0f, seconds});
}

void Input::setLightColor(const glm::vec3& rgb, int player) {
    std::lock_guard lock(m_feedbackMutex);
    m_feedback.push_back({Feedback::Kind::Light, player, rgb.r, rgb.g, rgb.b, 0.0});
}

void Input::applyFeedback() {
    std::vector<Feedback> queue;
    {
        std::lock_guard lock(m_feedbackMutex);
        queue.swap(m_feedback);
    }
    auto strength = [](float v) { return static_cast<Uint16>(std::clamp(v, 0.0f, 1.0f) * 65535.0f); };
    auto byte = [](float v) { return static_cast<Uint8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    for (const Feedback& f : queue) {
        for (size_t i = 0; i < m_devices.size(); ++i) {
            const InputDevice& d = m_devices[i];
            SDL_Gamepad* pad = m_handles[i].pad;
            if (!pad || (f.player >= 0 && d.player != f.player)) continue;
            const Uint32 ms = static_cast<Uint32>(std::max(0.0, f.seconds) * 1000.0);
            switch (f.kind) {
            case Feedback::Kind::Rumble: SDL_RumbleGamepad(pad, strength(f.a), strength(f.b), ms); break;
            case Feedback::Kind::TriggerRumble: SDL_RumbleGamepadTriggers(pad, strength(f.a), strength(f.b), ms); break;
            case Feedback::Kind::Light: SDL_SetGamepadLED(pad, byte(f.a), byte(f.b), byte(f.c)); break;
            }
        }
    }
}

std::string InputDevice::buttonLabel(GamepadButton button) const {
    const bool playstation =
        type == GamepadType::PlayStation3 || type == GamepadType::PlayStation4 || type == GamepadType::PlayStation5;
    if (playstation) {
        const char* ps[] = {
            "Cross", "Circle",  "Square", "Triangle", type == GamepadType::PlayStation5 ? "Create" : "Share",
            "PS",    "Options", "L3",     "R3",       "L1",
            "R1",    "Up",      "Down",   "Left",     "Right",
            "Mute",  "Touchpad"};
        return ps[static_cast<size_t>(button)];
    }
    if (type == GamepadType::Switch) {
        static const char* sw[] = {"B", "A", "Y",  "X",    "-",    "Home",  "+",       "L stick", "R stick",
                                   "L", "R", "Up", "Down", "Left", "Right", "Capture", "-"};
        return sw[static_cast<size_t>(button)];
    }
    static const char* xb[] = {"A",  "B",  "X",  "Y",    "View", "Xbox",  "Menu",  "LS", "RS",
                               "LB", "RB", "Up", "Down", "Left", "Right", "Share", "-"};
    return xb[static_cast<size_t>(button)];
}

}   // namespace shockwave
