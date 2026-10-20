#pragma once

// The local player's devices, turned into one InputCommand per fixed tick (plan 8.1). No
// World access: the same commands drive the local pawn offline and on the host, and go
// to the host from a client.
//
// collect() runs once per rendered frame, after the engine polled its devices: mouse
// movement turns the local aim exactly once, held buttons are latched, and a press waits
// for the next command. next() runs once per fixed tick and hands that press to exactly
// one command, however many ticks the frame runs (none or several).

#include <Network/Messages.h>

#include <glm/vec2.hpp>

#include <cstdint>

namespace shockwave {
class Input;
}

namespace arena {

// One frame's reading of the devices.
struct RawInput {
    float moveX = 0, moveZ = 0;   // -1..1: right and back are positive
    glm::vec2 mouse{0};   // pixels moved this frame
    bool fire = false, sprint = false;
    bool jumpPressed = false, reloadPressed = false;
    // False in menus, with the mouse free or the window unfocused: neutral movement and no
    // fire; presses are dropped, not saved for later.
    bool active = true;
};

// Arena's actions (Config/Input.swinput) for this frame.
RawInput readDevices(const shockwave::Input& input, bool active);

class InputSampler {
public:
    void collect(const RawInput& in, float sensitivity);
    // The command for the next fixed tick; target_tick is left for NetworkWorld to set.
    shockwave::net::InputCommand next(uint32_t incarnation);
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

// The same aim decoded from a command: what the authority simulates with.
float commandYaw(const shockwave::net::InputCommand& command);
float commandPitch(const shockwave::net::InputCommand& command);

}   // namespace arena
