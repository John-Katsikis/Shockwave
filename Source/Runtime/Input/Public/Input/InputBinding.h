#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shockwave {

// Gamepad buttons by position, so one binding works on every pad: South is Cross on a
// PlayStation pad and A on an Xbox pad, East is Circle or B, and so on.
enum class GamepadButton : uint8_t {
    South,
    East,
    West,
    North,
    Back,   // Share / Create on PlayStation, View on Xbox
    Guide,   // the PS or Xbox button
    Start,   // Options on PlayStation, Menu on Xbox
    LeftStick,
    RightStick,
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Misc,   // the DualSense mute button, Xbox Series share button
    Touchpad,   // clicking the PlayStation touchpad
    Count
};

// Sticks run -1..1 with up and right positive; triggers run 0..1.
enum class GamepadAxis : uint8_t { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count };

// What one binding listens to. Every binding gives a value from 0 to 1: buttons and keys
// are 0 or 1, an axis gives how far it is pushed. An action's value is its strongest
// binding, and it counts as held from 0.5.
struct InputBinding {
    enum class Source : uint8_t {
        Key,   // code: Key
        Mouse,   // code: MouseButton
        GamepadButton,   // code: GamepadButton, on any gamepad
        GamepadAxis,   // code: GamepadAxis, on any gamepad
        JoystickButton,   // code: button index on a joystick, wheel or HOTAS
        JoystickAxis,   // code: axis index
        JoystickHat,   // code: hat index * 4 + direction (0 up, 1 right, 2 down, 3 left)
    };
    // Which part of an axis counts: the half above or below the center, or the whole range
    // mapped to 0..1 (pedals and throttles that rest at one end).
    enum class Range : uint8_t { Positive, Negative, Full };

    Source source = Source::Key;
    uint16_t code = 0;
    Range range = Range::Positive;
    bool invert = false;   // axes only: flip before applying the range
    std::string device = "*";   // joysticks only: part of the device name, or * for any

    bool operator==(const InputBinding&) const = default;
};

// The text form used in .swinput files and the editor, e.g. "key W", "mouse right",
// "pad south", "pad_axis left_y +", "joy \"T.16000M\" axis 2 full invert",
// "joy * hat 0 up". docs/InputSchema.md (player --input-schema) lists every name.
std::string formatBinding(const InputBinding& binding);
bool parseBinding(std::string_view text, InputBinding& binding, std::string& error);

// A project's bindings: Config/Input.swinput next to its .swproject.
//
//   shockwave_input 1
//   deadzone 0.15
//   bind jump key Space
//   bind jump pad south
//   bind steer joy "G29" axis 0 +
//
// An action named in the file replaces that action's built-in bindings; others keep them.
struct InputBindings {
    float deadzone = 0.15f;   // stick and joystick axes: values below this read as 0
    std::vector<std::pair<std::string, InputBinding>> binds;   // action, binding; in file order
};

// Strict: unknown names and bad lines fail with "file:line: message", leaving out unchanged.
bool parseInputBindings(std::string_view text, InputBindings& out, std::string& error, std::string_view fileName = {});
bool loadInputBindings(const std::filesystem::path& path, InputBindings& out, std::string& error);
std::string formatInputBindings(const InputBindings& bindings);
bool saveInputBindings(const std::filesystem::path& path, const InputBindings& bindings, std::string& error);

// Where a project keeps its bindings.
inline std::filesystem::path inputBindingsPath(const std::filesystem::path& projectRoot) {
    return projectRoot / "Config" / "Input.swinput";
}

// Markdown reference of the file format and every key, button and axis name.
std::string inputReference();

// Names, for menus and messages. Empty for codes with no name.
std::string_view keyName(uint16_t key);
std::string_view gamepadButtonName(GamepadButton button);
std::string_view gamepadAxisName(GamepadAxis axis);

}   // namespace shockwave
