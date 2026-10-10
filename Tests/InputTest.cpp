// Input: the bindings text format, and gamepads and joysticks driven through SDL's
// virtual devices, so it runs anywhere without a controller plugged in.
#include <Input/Input.h>
#include <Input/InputBinding.h>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace shockwave;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static bool near(float a, float b) { return std::abs(a - b) < 0.02f; }

static void frame(Input& input) {
    input.beginFrame();
    SDL_Event event;
    while (SDL_PollEvent(&event)) input.handleEvent(event);
    input.update(1.0);
}

static void testFormat() {
    const char* forms[] = {
        "key W",
        "key Space",
        "mouse right",
        "pad south",
        "pad dpad_left",
        "pad_axis left_y +",
        "pad_axis right_trigger +",
        "pad_axis left_x - invert",
        "joy * button 3",
        "joy \"Thrustmaster T.16000M\" axis 2 full invert",
        "joy \"G29\" hat 0 left",
    };
    for (const char* text : forms) {
        InputBinding b;
        std::string error;
        CHECK(parseBinding(text, b, error));
        CHECK(formatBinding(b) == text);
        if (formatBinding(b) != text)
            std::fprintf(stderr, "  '%s' came back as '%s'\n", text, formatBinding(b).c_str());
    }
    InputBinding b;
    std::string error;
    CHECK(!parseBinding("key Banana", b, error) && error.find("Banana") != std::string::npos);
    CHECK(!parseBinding("pad_axis left_x", b, error));
    CHECK(!parseBinding("joy \"G29 axis 0 +", b, error));
    CHECK(parseBinding("PAD SOUTH", b, error) && b.source == InputBinding::Source::GamepadButton);
    CHECK(parseBinding("pad cross", b, error) && formatBinding(b) == "pad south");
    CHECK(parseBinding("pad Triangle", b, error) && formatBinding(b) == "pad north");

    InputBindings file;
    CHECK(parseInputBindings(
        "shockwave_input 1\ndeadzone 0.2\nbind jump key Space # comment\nbind jump pad south\n", file, error));
    CHECK(file.binds.size() == 2 && near(file.deadzone, 0.2f));
    InputBindings again;
    CHECK(parseInputBindings(formatInputBindings(file), again, error));
    CHECK(again.binds == file.binds);
    CHECK(!parseInputBindings("shockwave_input 1\n\nbind jump pad bogus\n", again, error, "Input.swinput"));
    CHECK(error.rfind("Input.swinput:3:", 0) == 0);
    CHECK(!parseInputBindings("bind jump key A\n", again, error));
    CHECK(inputReference().find("`south` | Cross | A") != std::string::npos);
}

static void testFileOverridesBuiltIn() {
    Input input;
    input.bindAction("jump", Key::Space);
    input.bindAction("fire", MouseButton::Left);
    InputBindings file;
    file.binds.push_back({"jump", {.source = InputBinding::Source::GamepadButton, .code = 0}});
    input.setBindings(file);
    CHECK(input.actions().at("jump").size() == 1);   // replaced
    CHECK(input.actions().at("fire").size() == 1);   // kept
    input.clearBindingsFile();
    CHECK(input.actions().at("jump").front().source == InputBinding::Source::Key);

    // Hot reload: a changed file applies on the next update; a broken one keeps the old.
    const auto path = std::filesystem::temp_directory_path() / "shockwave_input_test.swinput";
    { std::ofstream(path) << "shockwave_input 1\nbind jump key J\n"; }
    std::string error;
    CHECK(input.loadBindingsFile(path, error));
    CHECK(input.actions().at("jump").front().code == static_cast<uint16_t>(Key::J));
    { std::ofstream(path) << "shockwave_input 1\nbind jump key K\n"; }
    std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(2));
    input.update(1.0);
    CHECK(input.actions().at("jump").front().code == static_cast<uint16_t>(Key::K));
    { std::ofstream(path) << "shockwave_input 1\nbind jump nonsense\n"; }
    std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(4));
    input.update(1.0);
    CHECK(input.actions().at("jump").front().code == static_cast<uint16_t>(Key::K));
    CHECK(!input.bindingsError().empty());
    std::filesystem::remove(path);
}

static void testVirtualGamepad() {
    Input input;
    input.bindAction("jump", GamepadButton::South);
    input.bindAction("forward", GamepadAxis::LeftY, InputBinding::Range::Positive);
    input.bindAction("back", GamepadAxis::LeftY, InputBinding::Range::Negative);

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Virtual Pad";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    CHECK(id != 0);
    frame(input);
    CHECK(input.devices().size() == 1);
    if (input.devices().size() != 1) return;
    CHECK(input.devices()[0].kind == InputDevice::Kind::Gamepad && input.devices()[0].player == 0);
    CHECK(input.gamepad(0) != nullptr);
    SDL_Joystick* joy = SDL_GetJoystickFromID(id);
    CHECK(joy != nullptr);

    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    frame(input);
    CHECK(input.isActionDown("jump") && input.wasActionPressed("jump"));
    CHECK(input.lastPressed() && formatBinding(*input.lastPressed()) == "pad south");
    CHECK(!input.isActionDown("jump", KeyboardMouse));   // filtered out by source
    frame(input);
    CHECK(input.isActionDown("jump") && !input.wasActionPressed("jump"));
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    frame(input);
    CHECK(!input.isActionDown("jump") && input.wasActionReleased("jump"));

    // Stick up is positive; the deadzone is rescaled away.
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTY, -32767);
    frame(input);
    CHECK(near(input.axis("back", "forward"), 1.0f));
    CHECK(input.wasActionPressed("forward"));
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTY, 16384);   // half down
    frame(input);
    CHECK(input.axis("back", "forward") < -0.3f && input.axis("back", "forward") > -0.5f);
    CHECK(!input.isActionDown("back"));   // under halfway after the deadzone
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTY, 2000);   // inside the deadzone
    frame(input);
    CHECK(input.axis("back", "forward") == 0.0f);

    input.rumble(0.5f, 0.5f, 0.1);   // must not crash on a pad without rumble
    frame(input);
    SDL_DetachVirtualJoystick(id);
    frame(input);
    CHECK(input.devices().empty());
}

static void testVirtualJoystick() {
    Input input;
    InputBindings file;
    std::string error;
    CHECK(parseInputBindings(
        "shockwave_input 1\n"
        "bind throttle joy \"pedals\" axis 1 full\n"
        "bind fire joy * button 4\n"
        "bind look_left joy * hat 0 left\n"
        "bind wrong joy \"Other Device\" button 4\n",
        file, error));
    input.setBindings(file);

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_FLIGHT_STICK;
    desc.naxes = 3;
    desc.nbuttons = 8;
    desc.nhats = 1;
    desc.name = "Virtual Pedals And Stick";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    SDL_Joystick* opened = SDL_OpenJoystick(id);   // to set its virtual state
    SDL_SetJoystickVirtualAxis(opened, 1, -32768);   // a pedal resting at one end
    frame(input);
    CHECK(input.devices().size() == 1);
    if (input.devices().size() != 1) return;
    CHECK(input.devices()[0].kind == InputDevice::Kind::Joystick);
    CHECK(input.devices()[0].axes.size() == 3 && input.devices()[0].hats.size() == 1);
    CHECK(near(input.actionValue("throttle"), 0.0f));

    SDL_SetJoystickVirtualAxis(opened, 1, 0);
    frame(input);
    CHECK(near(input.actionValue("throttle"), 0.5f));
    // Halfway down its travel, Listen records it as the full travel of a pedal.
    CHECK(
        input.lastPressed() && input.lastPressed()->range == InputBinding::Range::Full && !input.lastPressed()->invert);
    SDL_SetJoystickVirtualAxis(opened, 1, 32767);
    frame(input);
    CHECK(near(input.actionValue("throttle"), 1.0f));

    SDL_SetJoystickVirtualButton(opened, 4, true);
    frame(input);
    CHECK(input.wasActionPressed("fire"));
    CHECK(!input.isActionDown("wrong"));   // the device name doesn't match
    CHECK(input.lastPressed() && input.lastPressed()->device == "Virtual Pedals And Stick");

    SDL_SetJoystickVirtualHat(opened, 0, SDL_HAT_LEFT);
    frame(input);
    CHECK(input.wasActionPressed("look_left"));
    CHECK(input.lastPressed() && formatBinding(*input.lastPressed()) == "joy \"Virtual Pedals And Stick\" hat 0 left");

    SDL_CloseJoystick(opened);
    SDL_DetachVirtualJoystick(id);
    frame(input);
}

int main() {
    if (!SDL_Init(SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    testFormat();
    testFileOverridesBuiltIn();
    testVirtualGamepad();
    testVirtualJoystick();
    SDL_Quit();
    if (failures) {
        std::fprintf(stderr, "%d input check(s) failed\n", failures);
        return 1;
    }
    std::puts("input: all checks passed");
    return 0;
}
