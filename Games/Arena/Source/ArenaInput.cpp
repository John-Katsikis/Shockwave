#include "ArenaInput.h"

#include <Input/Input.h>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>

namespace arena {
using shockwave::net::InputCommand;

RawInput readDevices(const shockwave::Input& input, bool active) {
    RawInput raw;
    raw.active = active;
    if (!active) return raw;
    raw.moveX = input.axis("arena_left", "arena_right");
    raw.moveZ = input.axis("arena_forward", "arena_back");
    raw.mouse = input.mouseDelta();
    raw.fire = input.isActionDown("arena_fire");
    raw.sprint = input.isActionDown("arena_sprint");
    raw.jumpPressed = input.wasActionPressed("arena_jump");
    raw.reloadPressed = input.wasActionPressed("arena_reload");
    return raw;
}

void InputSampler::collect(const RawInput& in, float sensitivity) {
    if (!in.active) {
        m_moveX = m_moveZ = 0;
        m_held = 0;
        m_pressed = 0;
        return;
    }
    m_yaw -= glm::radians(in.mouse.x * sensitivity);
    m_yaw = std::remainder(m_yaw, 2 * glm::pi<float>());
    m_pitch = std::clamp(m_pitch - glm::radians(in.mouse.y * sensitivity), -kMaxPitch, kMaxPitch);
    glm::vec2 move(std::isfinite(in.moveX) ? in.moveX : 0, std::isfinite(in.moveZ) ? in.moveZ : 0);
    if (glm::length(move) > 1) move = glm::normalize(move);
    m_moveX = int8_t(std::lround(std::clamp(move.x, -1.0f, 1.0f) * 127));
    m_moveZ = int8_t(std::lround(std::clamp(move.y, -1.0f, 1.0f) * 127));
    m_held = uint16_t((in.fire ? InputCommand::kHeldFire : 0) | (in.sprint ? InputCommand::kHeldSprint : 0));
    m_pressed |= uint16_t(
        (in.jumpPressed ? InputCommand::kPressedJump : 0) | (in.reloadPressed ? InputCommand::kPressedReload : 0));
}

InputCommand InputSampler::next(uint32_t incarnation) {
    InputCommand c;
    c.incarnation = incarnation;
    c.moveX = m_moveX;
    c.moveZ = m_moveZ;
    c.yaw = shockwave::net::encodeYaw(m_yaw);
    c.pitch = shockwave::net::encodePitch(m_pitch);
    c.heldBits = m_held;
    c.pressedBits = m_pressed;
    m_pressed = 0;   // one command gets each press
    return c;
}

void InputSampler::setAim(float yaw, float pitch) {
    m_yaw = std::isfinite(yaw) ? std::remainder(yaw, 2 * glm::pi<float>()) : 0;
    m_pitch = std::isfinite(pitch) ? std::clamp(pitch, -kMaxPitch, kMaxPitch) : 0;
}

void InputSampler::reset() { *this = InputSampler{}; }

float commandYaw(const InputCommand& command) { return shockwave::net::decodeYaw(command.yaw); }
float commandPitch(const InputCommand& command) { return shockwave::net::decodePitch(command.pitch); }

}   // namespace arena
