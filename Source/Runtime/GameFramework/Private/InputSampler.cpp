#include <GameFramework/InputSampler.h>

#include <Input/Input.h>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace shockwave {
using net::InputCommand;

InputFrame readInputFrame(const Input& input, const InputActionNames& names, bool active) {
    InputFrame frame;
    frame.active = active;
    if (!active) return frame;
    frame.moveX = input.axis(names.left, names.right);
    frame.moveZ = input.axis(names.forward, names.back);
    frame.look = input.mouseDelta();
    frame.fire = input.isActionDown(names.fire);
    frame.sprint = input.isActionDown(names.sprint);
    frame.jumpPressed = input.wasActionPressed(names.jump);
    frame.reloadPressed = input.wasActionPressed(names.reload);
    return frame;
}

void InputSampler::collect(const InputFrame& in, float sensitivity) {
    if (!in.active) {
        m_moveX = m_moveZ = 0;
        m_held = 0;
        m_pressed = 0;
        return;
    }
    const glm::vec2 look(std::isfinite(in.look.x) ? in.look.x : 0, std::isfinite(in.look.y) ? in.look.y : 0);
    m_yaw -= glm::radians(look.x * sensitivity);
    m_yaw = std::remainder(m_yaw, 2 * glm::pi<float>());
    m_pitch = std::clamp(m_pitch - glm::radians(look.y * sensitivity), -kMaxPitch, kMaxPitch);
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
    c.yaw = net::encodeYaw(m_yaw);
    c.pitch = net::encodePitch(m_pitch);
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

float commandYaw(const InputCommand& command) { return net::decodeYaw(command.yaw); }
float commandPitch(const InputCommand& command) { return net::decodePitch(command.pitch); }

glm::vec3 commandMoveDirection(const InputCommand& command) {
    glm::vec3 move(float(command.moveX) / 127, 0, float(command.moveZ) / 127);
    if (glm::length(move) > 1) move = glm::normalize(move);
    return glm::quat(glm::vec3(0, commandYaw(command), 0)) * move;
}

}   // namespace shockwave
