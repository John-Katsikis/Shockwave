#pragma once

// The local player's devices, turned into one InputCommand per fixed tick (docs/Networking.md,
// "Input and commands"; plan 8.1). No World access: the same commands drive the local
// character offline and on the host, and go to the host from a client, so a game's
// rules only ever read commands.
//
// collect() runs once per rendered frame, after the engine polled its devices (from
// GameMode::collectInput): mouse movement turns the local aim exactly once, held buttons
// are latched, and a press waits for the next command. next() runs once per fixed tick
// and hands that press to exactly one command, however many ticks the frame runs (none or
// several). A camera turns with aim() every frame; the simulation gets the aim sampled
// into each command.

#include <Network/Messages.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>

namespace shockwave {

class Input;

// One frame's reading of the devices.
struct InputFrame {
    float moveX = 0, moveZ = 0;   // -1..1: right and back are positive
    glm::vec2 look{0};   // mouse pixels (or stick units) moved this frame
    bool fire = false, sprint = false;
    bool jumpPressed = false, reloadPressed = false;
    // False in menus, with the mouse free or the window unfocused: neutral movement and no
    // fire; presses are dropped, not saved for later.
    bool active = true;
};

// The game's action names (Config/Input.swinput) for each part of a command.
struct InputActionNames {
    const char* left = "move_left";
    const char* right = "move_right";
    const char* forward = "move_forward";
    const char* back = "move_back";
    const char* fire = "fire";
    const char* sprint = "sprint";
    const char* jump = "jump";
    const char* reload = "reload";
};

// This frame's actions; neutral when !active.
InputFrame readInputFrame(const Input& input, const InputActionNames& names, bool active);

class InputSampler {
public:
    // sensitivity: degrees of turn per unit of look.
    void collect(const InputFrame& frame, float sensitivity);
    // The command for the next fixed tick; NetworkWorld::sendInput sets target_tick.
    net::InputCommand next(uint32_t incarnation);
    // The local aim, for a camera that turns every frame.
    float yaw() const { return m_yaw; }
    float pitch() const { return m_pitch; }
    void setAim(float yaw, float pitch);
    void reset();

    static constexpr float kMaxPitch = 1.45f;   // about 83 degrees; the wire allows 89

private:
    float m_yaw = 0, m_pitch = 0;
    int8_t m_moveX = 0, m_moveZ = 0;
    uint16_t m_held = 0, m_pressed = 0;
};

// What the authority simulates with: the aim decoded from a command, and its movement as
// a world-space direction of length 0..1 (turned by the command's yaw).
float commandYaw(const net::InputCommand& command);
float commandPitch(const net::InputCommand& command);
glm::vec3 commandMoveDirection(const net::InputCommand& command);

}   // namespace shockwave
