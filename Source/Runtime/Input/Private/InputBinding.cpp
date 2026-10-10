#include <Input/InputBinding.h>

#include <Input/Input.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>

namespace shockwave {
namespace {

struct KeyName {
    uint16_t code;
    std::string_view name;
};

const std::vector<KeyName>& keyNames() {
    static const std::vector<KeyName> names = [] {
        static const char* letters[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                        "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
        static const char* digits[] = {"Num1", "Num2", "Num3", "Num4", "Num5", "Num6", "Num7", "Num8", "Num9", "Num0"};
        static const char* fkeys[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
        std::vector<KeyName> v;
        for (int i = 0; i < 26; ++i) v.push_back({static_cast<uint16_t>(static_cast<int>(Key::A) + i), letters[i]});
        for (int i = 0; i < 10; ++i) v.push_back({static_cast<uint16_t>(static_cast<int>(Key::Num1) + i), digits[i]});
        for (int i = 0; i < 12; ++i) v.push_back({static_cast<uint16_t>(static_cast<int>(Key::F1) + i), fkeys[i]});
        const std::pair<Key, const char*> rest[] = {
            {Key::Enter, "Enter"},
            {Key::Escape, "Escape"},
            {Key::Backspace, "Backspace"},
            {Key::Tab, "Tab"},
            {Key::Space, "Space"},
            {Key::Right, "Right"},
            {Key::Left, "Left"},
            {Key::Down, "Down"},
            {Key::Up, "Up"},
            {Key::LeftCtrl, "LeftCtrl"},
            {Key::LeftShift, "LeftShift"},
            {Key::LeftAlt, "LeftAlt"},
            {Key::RightCtrl, "RightCtrl"},
            {Key::RightShift, "RightShift"},
            {Key::RightAlt, "RightAlt"},
        };
        for (const auto& [key, name] : rest) v.push_back({static_cast<uint16_t>(key), name});
        return v;
    }();
    return names;
}

constexpr std::array<std::string_view, 6> kMouseNames = {"", "left", "middle", "right", "x1", "x2"};
constexpr std::array<std::string_view, static_cast<size_t>(GamepadButton::Count)> kButtonNames = {
    "south",     "east",       "west",        "north",         "back",           "guide",
    "start",     "left_stick", "right_stick", "left_shoulder", "right_shoulder", "dpad_up",
    "dpad_down", "dpad_left",  "dpad_right",  "misc",          "touchpad"};
constexpr std::array<std::string_view, static_cast<size_t>(GamepadAxis::Count)> kAxisNames = {
    "left_x", "left_y", "right_x", "right_y", "left_trigger", "right_trigger"};
constexpr std::array<std::string_view, 4> kHatNames = {"up", "right", "down", "left"};

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

template <size_t N>
int indexOf(const std::array<std::string_view, N>& names, std::string_view name) {
    for (size_t i = 0; i < N; ++i)
        if (!names[i].empty() && iequals(names[i], name)) return static_cast<int>(i);
    return -1;
}

// Splits on spaces; "double quotes" keep spaces in device names.
bool tokenize(std::string_view text, std::vector<std::string>& out, std::string& error) {
    out.clear();
    size_t i = 0;
    while (i < text.size()) {
        if (std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
            continue;
        }
        if (text[i] == '"') {
            const size_t end = text.find('"', i + 1);
            if (end == std::string_view::npos) {
                error = "missing closing quote";
                return false;
            }
            out.emplace_back(text.substr(i + 1, end - i - 1));
            i = end + 1;
            continue;
        }
        size_t end = i;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
        out.emplace_back(text.substr(i, end - i));
        i = end;
    }
    return true;
}

bool parseIndex(const std::string& s, int max, uint16_t& out) {
    int v = -1;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || p != s.data() + s.size() || v < 0 || v > max) return false;
    out = static_cast<uint16_t>(v);
    return true;
}

bool parseRange(const std::vector<std::string>& t, size_t at, InputBinding& b, std::string& error) {
    if (at >= t.size()) {
        error = "expected + (positive half), - (negative half) or full";
        return false;
    }
    if (t[at] == "+")
        b.range = InputBinding::Range::Positive;
    else if (t[at] == "-")
        b.range = InputBinding::Range::Negative;
    else if (iequals(t[at], "full"))
        b.range = InputBinding::Range::Full;
    else {
        error = "unknown axis range '" + t[at] + "' (use +, - or full)";
        return false;
    }
    if (at + 1 < t.size()) {
        if (!iequals(t[at + 1], "invert")) {
            error = "unexpected '" + t[at + 1] + "' (only 'invert' may follow the range)";
            return false;
        }
        b.invert = true;
        if (at + 2 < t.size()) {
            error = "unexpected '" + t[at + 2] + "'";
            return false;
        }
    }
    return true;
}

std::string quoteDevice(const std::string& device) {
    if (device == "*") return "*";
    return "\"" + device + "\"";
}

}   // namespace

std::string_view keyName(uint16_t key) {
    for (const KeyName& k : keyNames())
        if (k.code == key) return k.name;
    return {};
}
std::string_view gamepadButtonName(GamepadButton b) {
    return static_cast<size_t>(b) < kButtonNames.size() ? kButtonNames[static_cast<size_t>(b)] : std::string_view{};
}
std::string_view gamepadAxisName(GamepadAxis a) {
    return static_cast<size_t>(a) < kAxisNames.size() ? kAxisNames[static_cast<size_t>(a)] : std::string_view{};
}

std::string formatBinding(const InputBinding& b) {
    using S = InputBinding::Source;
    const char* range = b.range == InputBinding::Range::Positive   ? "+"
                        : b.range == InputBinding::Range::Negative ? "-"
                                                                   : "full";
    const std::string tail = std::string(" ") + range + (b.invert ? " invert" : "");
    switch (b.source) {
    case S::Key: return "key " + std::string(keyName(b.code));
    case S::Mouse: return "mouse " + std::string(b.code < kMouseNames.size() ? kMouseNames[b.code] : "");
    case S::GamepadButton: return "pad " + std::string(gamepadButtonName(static_cast<GamepadButton>(b.code)));
    case S::GamepadAxis: return "pad_axis " + std::string(gamepadAxisName(static_cast<GamepadAxis>(b.code))) + tail;
    case S::JoystickButton: return "joy " + quoteDevice(b.device) + " button " + std::to_string(b.code);
    case S::JoystickAxis: return "joy " + quoteDevice(b.device) + " axis " + std::to_string(b.code) + tail;
    case S::JoystickHat:
        return "joy " + quoteDevice(b.device) + " hat " + std::to_string(b.code / 4) + " " +
               std::string(kHatNames[b.code % 4]);
    }
    return {};
}

bool parseBinding(std::string_view text, InputBinding& out, std::string& error) {
    std::vector<std::string> t;
    if (!tokenize(text, t, error)) return false;
    if (t.empty()) {
        error = "empty binding";
        return false;
    }
    InputBinding b;
    const std::string& kind = t[0];
    auto expect = [&](size_t count, const char* usage) {
        if (t.size() == count) return true;
        error = std::string("expected '") + usage + "'";
        return false;
    };
    using S = InputBinding::Source;
    if (iequals(kind, "key")) {
        if (!expect(2, "key <name>")) return false;
        const auto& names = keyNames();
        auto it = std::find_if(names.begin(), names.end(), [&](const KeyName& k) { return iequals(k.name, t[1]); });
        if (it == names.end()) {
            error = "unknown key '" + t[1] + "'";
            return false;
        }
        b.source = S::Key;
        b.code = it->code;
    } else if (iequals(kind, "mouse")) {
        if (!expect(2, "mouse left|right|middle|x1|x2")) return false;
        const int i = indexOf(kMouseNames, t[1]);
        if (i < 0) {
            error = "unknown mouse button '" + t[1] + "'";
            return false;
        }
        b.source = S::Mouse;
        b.code = static_cast<uint16_t>(i);
    } else if (iequals(kind, "pad")) {
        if (!expect(2, "pad <button>")) return false;
        int i = indexOf(kButtonNames, t[1]);
        // PlayStation and Xbox names work too, and are written back by position.
        static const std::pair<std::string_view, GamepadButton> aliases[] = {
            {"cross", GamepadButton::South},      {"a", GamepadButton::South},
            {"circle", GamepadButton::East},      {"b", GamepadButton::East},
            {"square", GamepadButton::West},      {"x", GamepadButton::West},
            {"triangle", GamepadButton::North},   {"y", GamepadButton::North},
            {"l1", GamepadButton::LeftShoulder},  {"lb", GamepadButton::LeftShoulder},
            {"r1", GamepadButton::RightShoulder}, {"rb", GamepadButton::RightShoulder},
            {"l3", GamepadButton::LeftStick},     {"r3", GamepadButton::RightStick},
            {"options", GamepadButton::Start},    {"menu", GamepadButton::Start},
            {"share", GamepadButton::Back},       {"create", GamepadButton::Back},
            {"view", GamepadButton::Back}};
        for (const auto& [name, button] : aliases)
            if (i < 0 && iequals(name, t[1])) i = static_cast<int>(button);
        if (i < 0) {
            error = "unknown gamepad button '" + t[1] + "'";
            return false;
        }
        b.source = S::GamepadButton;
        b.code = static_cast<uint16_t>(i);
    } else if (iequals(kind, "pad_axis")) {
        if (t.size() < 2) {
            error = "expected 'pad_axis <axis> +|-|full'";
            return false;
        }
        const int i = indexOf(kAxisNames, t[1]);
        if (i < 0) {
            error = "unknown gamepad axis '" + t[1] + "'";
            return false;
        }
        b.source = S::GamepadAxis;
        b.code = static_cast<uint16_t>(i);
        if (!parseRange(t, 2, b, error)) return false;
    } else if (iequals(kind, "joy")) {
        if (t.size() < 4) {
            error = "expected 'joy <device> button <n>', 'joy <device> axis <n> +|-|full' or 'joy <device> hat <n> "
                    "up|down|left|right'";
            return false;
        }
        b.device = t[1];
        if (b.device.empty()) {
            error = "empty device name (use * for any joystick)";
            return false;
        }
        const std::string& what = t[2];
        if (iequals(what, "button")) {
            if (!expect(4, "joy <device> button <n>")) return false;
            b.source = S::JoystickButton;
            if (!parseIndex(t[3], 255, b.code)) {
                error = "button must be a number from 0 to 255";
                return false;
            }
        } else if (iequals(what, "axis")) {
            b.source = S::JoystickAxis;
            if (!parseIndex(t[3], 63, b.code)) {
                error = "axis must be a number from 0 to 63";
                return false;
            }
            if (!parseRange(t, 4, b, error)) return false;
        } else if (iequals(what, "hat")) {
            if (!expect(5, "joy <device> hat <n> up|down|left|right")) return false;
            uint16_t hat = 0;
            if (!parseIndex(t[3], 15, hat)) {
                error = "hat must be a number from 0 to 15";
                return false;
            }
            const int dir = indexOf(kHatNames, t[4]);
            if (dir < 0) {
                error = "unknown hat direction '" + t[4] + "' (use up, down, left or right)";
                return false;
            }
            b.source = S::JoystickHat;
            b.code = static_cast<uint16_t>(hat * 4 + dir);
        } else {
            error = "unknown joystick control '" + what + "' (use button, axis or hat)";
            return false;
        }
    } else {
        error = "unknown input '" + kind + "' (use key, mouse, pad, pad_axis or joy)";
        return false;
    }
    out = std::move(b);
    return true;
}

bool parseInputBindings(std::string_view text, InputBindings& out, std::string& error, std::string_view fileName) {
    InputBindings result;
    const std::string where = fileName.empty() ? std::string("input") : std::string(fileName);
    std::istringstream in{std::string(text)};
    std::string line;
    int lineNo = 0;
    bool sawHeader = false;
    auto fail = [&](const std::string& message) {
        error = where + ":" + std::to_string(lineNo) + ": " + message;
        return false;
    };
    while (std::getline(in, line)) {
        ++lineNo;
        if (const size_t hash = line.find('#'); hash != std::string::npos) {
            // A # inside a quoted device name is part of the name.
            const size_t quotes =
                static_cast<size_t>(std::count(line.begin(), line.begin() + static_cast<long>(hash), '"'));
            if (quotes % 2 == 0) line.resize(hash);
        }
        std::istringstream words(line);
        std::string keyword;
        if (!(words >> keyword)) continue;
        if (!sawHeader) {
            int version = 0;
            if (keyword != "shockwave_input" || !(words >> version)) return fail("expected 'shockwave_input 1' first");
            if (version != 1) return fail("unsupported version " + std::to_string(version) + " (this engine reads 1)");
            sawHeader = true;
            continue;
        }
        if (keyword == "deadzone") {
            float dz = -1;
            if (!(words >> dz) || dz < 0.0f || dz >= 1.0f) return fail("deadzone must be a number from 0 to 0.99");
            result.deadzone = dz;
        } else if (keyword == "bind") {
            std::string action;
            if (!(words >> action)) return fail("expected 'bind <action> <input>'");
            std::string rest;
            std::getline(words, rest);
            InputBinding b;
            std::string bindError;
            if (!parseBinding(rest, b, bindError)) return fail(bindError);
            result.binds.emplace_back(action, std::move(b));
        } else {
            return fail("unknown keyword '" + keyword + "' (use bind or deadzone)");
        }
    }
    if (!sawHeader) {
        error = where + ": empty file (expected 'shockwave_input 1')";
        return false;
    }
    out = std::move(result);
    return true;
}

bool loadInputBindings(const std::filesystem::path& path, InputBindings& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = path.string() + ": can't open";
        return false;
    }
    std::stringstream text;
    text << file.rdbuf();
    return parseInputBindings(text.str(), out, error, path.string());
}

std::string formatInputBindings(const InputBindings& bindings) {
    std::ostringstream out;
    out << "# Input bindings: one 'bind <action> <input>' per line. An action can have many.\n"
           "# Names are listed in docs/InputSchema.md. Edit here or in the editor (Window > Input).\n"
           "shockwave_input 1\n";
    char dz[32];
    std::snprintf(dz, sizeof dz, "%g", static_cast<double>(bindings.deadzone));
    out << "deadzone " << dz << "\n";
    std::string previous;
    for (const auto& [action, binding] : bindings.binds) {
        if (action != previous) out << "\n";
        previous = action;
        out << "bind " << action << " " << formatBinding(binding) << "\n";
    }
    return out.str();
}

bool saveInputBindings(const std::filesystem::path& path, const InputBindings& bindings, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path temp = path.string() + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = temp.string() + ": can't write";
            return false;
        }
        file << formatInputBindings(bindings);
        if (!file) {
            error = temp.string() + ": write failed";
            return false;
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        error = path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

std::string inputReference() {
    std::ostringstream out;
    out << "# Input schema\n\n"
           "Generated by `player --input-schema`; don't edit by hand.\n\n"
           "A project's controls live in `Config/Input.swinput`, next to its `.swproject`. The editor\n"
           "edits the same file (Window > Input) and reloads it when it changes on disk.\n"
           "`player --check` validates it, and `player --devices` lists the controllers plugged in.\n\n"
           "```\n"
           "shockwave_input 1\n"
           "deadzone 0.15                  # sticks and joystick axes read 0 below this\n"
           "bind jump key Space\n"
           "bind jump pad south            # Cross on PlayStation, A on Xbox\n"
           "bind steer_right pad_axis left_x +\n"
           "bind throttle joy \"T.16000M\" axis 2 full invert\n"
           "bind brake joy \"G29\" axis 3 full\n"
           "```\n\n"
           "Each `bind` adds one input to an action; an action can have any number. An action named\n"
           "in the file replaces the engine's built-in bindings for it. Game code reads actions, never\n"
           "devices: `isActionDown`, `wasActionPressed`, `actionValue` (0..1) and `axis(neg, pos)` (-1..1).\n\n"
           "## Inputs\n\n"
           "| Form | Meaning |\n|---|---|\n"
           "| `key <name>` | a keyboard key, by position (WASD stays WASD on any layout) |\n"
           "| `mouse <button>` | a mouse button |\n"
           "| `pad <button>` | a button on any gamepad (PlayStation, Xbox, Switch, Steam...) |\n"
           "| `pad_axis <axis> <range> [invert]` | a gamepad stick or trigger |\n"
           "| `joy <device> button <n>` | a button on another controller: HOTAS, wheel, pedals, arcade stick |\n"
           "| `joy <device> axis <n> <range> [invert]` | an axis on one |\n"
           "| `joy <device> hat <n> <up/down/left/right>` | a hat switch direction |\n\n"
           "`<device>` is part of the controller's name as `player --devices` prints it, in quotes if it\n"
           "has spaces, or `*` for any. Joystick buttons and axes count from 0.\n\n"
           "`<range>` picks the part of an axis that counts: `+` the half above center, `-` the half\n"
           "below, `full` the whole travel as 0..1 (for pedals and throttles that rest at one end).\n"
           "`invert` flips the axis first. Stick Y is positive up.\n\n";
    out << "## Gamepad buttons\n\nButtons are named by position so one binding works on every pad. The PlayStation\n"
           "and Xbox names (`cross`, `a`, `circle`, `b`, `square`, `x`, `triangle`, `y`, `l1`, `lb`, `r1`, `rb`,\n"
           "`l3`, `r3`, `options`, `menu`, `share`, `create`, `view`) are accepted too and saved by position.\n\n"
           "| Name | PlayStation | Xbox |\n|---|---|---|\n";
    const char* ps[] = {
        "Cross", "Circle",   "Square",     "Triangle",   "Share / Create", "PS",   "Options",       "L3", "R3", "L1",
        "R1",    "D-pad up", "D-pad down", "D-pad left", "D-pad right",    "Mute", "Touchpad click"};
    const char* xb[] = {
        "A",
        "B",
        "X",
        "Y",
        "View",
        "Xbox",
        "Menu",
        "Left stick click",
        "Right stick click",
        "LB",
        "RB",
        "D-pad up",
        "D-pad down",
        "D-pad left",
        "D-pad right",
        "Share",
        "-"};
    for (size_t i = 0; i < kButtonNames.size(); ++i)
        out << "| `" << kButtonNames[i] << "` | " << ps[i] << " | " << xb[i] << " |\n";
    out << "\n## Gamepad axes\n\n| Name | Range |\n|---|---|\n";
    for (size_t i = 0; i < kAxisNames.size(); ++i)
        out << "| `" << kAxisNames[i] << "` | " << (i < 4 ? "-1..1 (left/down negative)" : "0..1 (use `+`)") << " |\n";
    out << "\n## Mouse buttons\n\n";
    for (size_t i = 1; i < kMouseNames.size(); ++i) out << (i > 1 ? ", " : "") << "`" << kMouseNames[i] << "`";
    out << "\n\n## Keys\n\n";
    bool first = true;
    for (const KeyName& k : keyNames()) {
        out << (first ? "" : ", ") << "`" << k.name << "`";
        first = false;
    }
    out << "\n";
    return out.str();
}

}   // namespace shockwave
