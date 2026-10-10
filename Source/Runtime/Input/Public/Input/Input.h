#pragma once

#include <Input/InputBinding.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

union SDL_Event;
struct SDL_Gamepad;
struct SDL_Joystick;

namespace shockwave {

class Window;

// Physical key positions (USB HID usage codes), so WASD stays WASD on any keyboard layout.
enum class Key : uint16_t {
    Unknown = 0,
    A = 4,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Num1 = 30,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,
    Num0,
    Enter = 40,
    Escape = 41,
    Backspace = 42,
    Tab = 43,
    Space = 44,
    F1 = 58,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    Right = 79,
    Left = 80,
    Down = 81,
    Up = 82,
    LeftCtrl = 224,
    LeftShift = 225,
    LeftAlt = 226,
    RightCtrl = 228,
    RightShift = 229,
    RightAlt = 230,
};

enum class MouseButton : uint8_t { Left = 1, Middle = 2, Right = 3, X1 = 4, X2 = 5 };

// Which kinds of device a query listens to.
enum InputSources : uint8_t { KeyboardMouse = 1, Gamepads = 2, Joysticks = 4, AllInputSources = 7 };

enum class GamepadType : uint8_t { Unknown, Xbox, PlayStation5, PlayStation4, PlayStation3, Switch, Steam, Other };

// A connected controller. Gamepads are anything SDL knows the layout of (PlayStation,
// Xbox, Switch, Steam and most third-party pads); everything else is a joystick: flight
// sticks, throttles, wheels, pedals, button boxes.
struct InputDevice {
    enum class Kind : uint8_t { Gamepad, Joystick };
    uint32_t id = 0;   // stays the same while it's plugged in
    Kind kind = Kind::Joystick;
    std::string name;
    GamepadType type = GamepadType::Unknown;
    int player = -1;   // gamepads: 0 for the first connected, 1 for the next... (lights the pad's player LED)
    // Gamepads: GamepadAxis order, sticks -1..1 with up and right positive, triggers 0..1.
    // Joysticks: every axis as the device reports it, -1..1.
    std::vector<float> axes;
    std::vector<uint8_t> buttons;   // 1 while held. Gamepads: GamepadButton order
    std::vector<uint8_t> hats;   // joysticks: bit 1 up, 2 right, 4 down, 8 left
    glm::vec3 gyro{0.0f};   // radians per second, when hasGyro
    glm::vec3 accel{0.0f};   // m/s², when hasGyro
    bool hasRumble = false, hasTriggerRumble = false, hasLight = false, hasGyro = false;

    // The name printed on the button: "Cross" on PlayStation, "A" on Xbox.
    std::string buttonLabel(GamepadButton button) const;
};

// Keyboard, mouse, gamepads and joysticks, behind named actions that games bind inputs to.
// Game code asks "is jump held?" rather than "is Space held?", so controls can be remapped
// in Config/Input.swinput (see InputBinding.h) or the editor without touching gameplay.
//
// Threading: the engine updates input on the main thread before the frame's hooks run.
// Reading it from any thread during the frame is safe, and so are rumble() and
// setLightColor(), which are applied on the next update. Binding is main-thread only.
class Input {
public:
    explicit Input(Window* window = nullptr);   // null: no mouse capture (tools, tests)
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    // Raw state. "Pressed" and "released" are true only on the frame the change happened.
    bool isKeyDown(Key key) const;
    bool wasKeyPressed(Key key) const;
    bool wasKeyReleased(Key key) const;
    bool isMouseDown(MouseButton button) const;
    bool wasMousePressed(MouseButton button) const;
    bool wasMouseReleased(MouseButton button) const;
    glm::vec2 mouseDelta() const { return m_mouseDelta; }   // pixels moved this frame
    glm::vec2 mousePosition() const { return m_mousePosition; }
    float scrollDelta() const { return m_scrollDelta; }

    // Built-in bindings, set up in code. The project's bindings file replaces these per
    // action (loadBindingsFile).
    void bindAction(const std::string& action, Key key);
    void bindAction(const std::string& action, MouseButton button);
    void bindAction(const std::string& action, GamepadButton button);
    void bindAction(const std::string& action, GamepadAxis axis, InputBinding::Range range);
    void bindAction(const std::string& action, const InputBinding& binding);
    void clearAction(const std::string& action);

    // Actions. Value is the strongest binding, 0..1; held means value >= 0.5.
    bool isActionDown(const std::string& action, uint8_t sources = AllInputSources) const;
    bool wasActionPressed(const std::string& action, uint8_t sources = AllInputSources) const;
    bool wasActionReleased(const std::string& action, uint8_t sources = AllInputSources) const;
    float actionValue(const std::string& action, uint8_t sources = AllInputSources) const;
    // -1..1 from a pair of actions, e.g. axis("move_left", "move_right"). Analog on sticks.
    float axis(const std::string& negative, const std::string& positive, uint8_t sources = AllInputSources) const;

    // Every action and its bindings as they apply now (built-in, then the file's).
    const std::map<std::string, std::vector<InputBinding>>& actions() const { return m_actions; }
    InputBindings currentBindings() const;   // the same, as a bindings file would hold them
    float deadzone() const { return m_deadzone; }

    // The project's bindings. Loading remembers the path and reloads when the file changes.
    // A file that fails to load leaves the previous bindings in place.
    bool loadBindingsFile(const std::filesystem::path& path, std::string& error);
    void setBindings(const InputBindings& bindings);   // what the editor applies before saving
    void clearBindingsFile();   // back to the built-in bindings only
    const std::filesystem::path& bindingsFile() const { return m_bindingsPath; }
    const std::string& bindingsError() const { return m_bindingsError; }   // the last reload's
    uint32_t bindingsVersion() const { return m_bindingsVersion; }   // bumps on every change

    // Controllers, in connection order.
    const std::vector<InputDevice>& devices() const { return m_devices; }
    const InputDevice* gamepad(int player) const;   // null if no pad has that player number
    // The first input pressed this frame, as a binding: what the editor's Listen button
    // records. Joystick axes count once they move halfway from where they rested.
    const std::optional<InputBinding>& lastPressed() const { return m_lastPressed; }

    // Feedback, from any thread. player -1: every gamepad. Strengths 0..1.
    void rumble(float low, float high, double seconds, int player = -1);
    void rumbleTriggers(float left, float right, double seconds, int player = -1);   // Xbox One/Series, DualSense
    void setLightColor(const glm::vec3& rgb, int player = -1);   // DualShock 4 light bar, DualSense

    // Captured: cursor hidden and locked to the window, mouseDelta keeps working at the edges.
    void setMouseCaptured(bool captured);
    bool isMouseCaptured() const { return m_mouseCaptured; }

    // Called by the engine: beginFrame, then handleEvent for each event, then update.
    void beginFrame();
    void handleEvent(const SDL_Event& event);
    void update(double frameDt);

private:
    enum Flags : uint8_t { Down = 1, Pressed = 2, Released = 4 };
    struct Handles {
        SDL_Gamepad* pad = nullptr;
        SDL_Joystick* joystick = nullptr;
        std::vector<uint8_t> buttonFlags;   // Flags, set from events so short taps aren't missed
        std::vector<float> previousAxes, restAxes;
        std::vector<uint8_t> previousHats;
    };
    struct Feedback {
        enum class Kind : uint8_t { Rumble, TriggerRumble, Light } kind;
        int player;
        float a, b, c;
        double seconds;
    };

    float bindingValue(const InputBinding& binding, bool previous, uint8_t sources) const;
    uint8_t actionState(const std::string& action, uint8_t sources) const;
    void rebuildActions();
    void openDevice(uint32_t id);
    void closeDevice(uint32_t id);
    int deviceIndex(uint32_t id) const;
    void pollDevices();
    void applyFeedback();
    void notePressed(const InputBinding& binding);
    void checkBindingsFile(double frameDt);

    Window* m_window;
    std::array<uint8_t, 512> m_keys{};
    std::array<uint8_t, 8> m_mouse{};
    glm::vec2 m_mouseDelta{0.0f};
    glm::vec2 m_mousePosition{0.0f};
    float m_scrollDelta = 0.0f;
    bool m_mouseCaptured = false;

    std::map<std::string, std::vector<InputBinding>> m_builtIn, m_actions;
    InputBindings m_file;
    bool m_hasFile = false;
    float m_deadzone = 0.15f;
    std::filesystem::path m_bindingsPath;
    std::filesystem::file_time_type m_bindingsTime{};
    double m_bindingsCheckTimer = 0.0;
    std::string m_bindingsError;
    uint32_t m_bindingsVersion = 0;

    std::vector<InputDevice> m_devices;
    std::vector<Handles> m_handles;   // parallel to m_devices
    std::optional<InputBinding> m_lastPressed;

    std::mutex m_feedbackMutex;
    std::vector<Feedback> m_feedback;
};

}   // namespace shockwave
