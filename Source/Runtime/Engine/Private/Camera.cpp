#include <Engine/Camera.h>

#include <Input/Input.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace shockwave {

glm::mat4 perspectiveReverseZ(float fovYRadians, float aspect, float nearPlane) {
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    glm::mat4 m(0.0f);
    m[0][0] = f / aspect;
    m[1][1] = f;
    m[2][3] = -1.0f;   // w = -z
    m[3][2] = nearPlane;   // z = near, so depth = near / -z
    return m;
}

void FlyCamera::bindDefaultControls(Input& input) {
    input.bindAction("camera_forward", Key::W);
    input.bindAction("camera_back", Key::S);
    input.bindAction("camera_left", Key::A);
    input.bindAction("camera_right", Key::D);
    input.bindAction("camera_up", Key::E);
    input.bindAction("camera_up", Key::Space);
    input.bindAction("camera_down", Key::Q);
    input.bindAction("camera_down", Key::LeftCtrl);
    input.bindAction("camera_fast", Key::LeftShift);
    input.bindAction("camera_look", MouseButton::Right);

    using R = InputBinding::Range;
    input.bindAction("camera_forward", GamepadAxis::LeftY, R::Positive);
    input.bindAction("camera_back", GamepadAxis::LeftY, R::Negative);
    input.bindAction("camera_left", GamepadAxis::LeftX, R::Negative);
    input.bindAction("camera_right", GamepadAxis::LeftX, R::Positive);
    input.bindAction("camera_up", GamepadAxis::RightTrigger, R::Positive);
    input.bindAction("camera_down", GamepadAxis::LeftTrigger, R::Positive);
    input.bindAction("camera_fast", GamepadButton::LeftStick);
    input.bindAction("camera_turn_left", GamepadAxis::RightX, R::Negative);
    input.bindAction("camera_turn_right", GamepadAxis::RightX, R::Positive);
    input.bindAction("camera_turn_up", GamepadAxis::RightY, R::Positive);
    input.bindAction("camera_turn_down", GamepadAxis::RightY, R::Negative);
}

void FlyCamera::update(Input& input, double frameDt, uint8_t sources) {
    const float dt = static_cast<float>(frameDt);
    const bool looking = (sources & KeyboardMouse) && input.isActionDown("camera_look", sources);
    input.setMouseCaptured(looking);
    if (looking) {
        const glm::vec2 delta = input.mouseDelta();
        yaw -= delta.x * lookSensitivity;
        pitch -= delta.y * lookSensitivity;
    }
    // Sticks turn at a rate rather than by a distance.
    yaw -= input.axis("camera_turn_left", "camera_turn_right", sources) * turnSpeed * dt;
    pitch += input.axis("camera_turn_down", "camera_turn_up", sources) * turnSpeed * dt;
    pitch = std::clamp(pitch, glm::radians(-89.0f), glm::radians(89.0f));

    glm::vec3 move = forward() * input.axis("camera_back", "camera_forward", sources) +
                     right() * input.axis("camera_left", "camera_right", sources) +
                     glm::vec3(0.0f, 1.0f, 0.0f) * input.axis("camera_down", "camera_up", sources);
    // Keys give full speed in any direction; a half-pushed stick gives half speed.
    const float amount = std::min(1.0f, glm::length(move));
    if (amount > 0.0f) {
        float speed = moveSpeed * (input.isActionDown("camera_fast", sources) ? fastMultiplier : 1.0f);
        position += glm::normalize(move) * amount * speed * dt;
    }

    // Scroll changes speed, like most editors.
    if ((sources & KeyboardMouse) && input.scrollDelta() != 0.0f) {
        moveSpeed = std::clamp(moveSpeed * std::pow(1.2f, input.scrollDelta()), 0.5f, 500.0f);
    }
}

void FlyCamera::lookAt(const glm::vec3& target) {
    const glm::vec3 d = glm::normalize(target - position);
    pitch = std::asin(std::clamp(d.y, -1.0f, 1.0f));
    yaw = std::atan2(-d.x, -d.z);
}

glm::vec3 FlyCamera::forward() const {
    return {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

glm::vec3 FlyCamera::right() const { return glm::normalize(glm::cross(forward(), glm::vec3(0.0f, 1.0f, 0.0f))); }

glm::mat4 FlyCamera::view() const { return glm::lookAt(position, position + forward(), glm::vec3(0.0f, 1.0f, 0.0f)); }

glm::mat4 FlyCamera::projection(float aspect) const { return perspectiveReverseZ(fovY, aspect, nearPlane); }

}   // namespace shockwave
