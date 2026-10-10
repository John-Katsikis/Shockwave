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
}

void FlyCamera::update(Input& input, double frameDt) {
    const bool looking = input.isActionDown("camera_look");
    input.setMouseCaptured(looking);
    if (looking) {
        const glm::vec2 delta = input.mouseDelta();
        yaw -= delta.x * lookSensitivity;
        pitch -= delta.y * lookSensitivity;
        pitch = std::clamp(pitch, glm::radians(-89.0f), glm::radians(89.0f));
    }

    glm::vec3 move = forward() * input.axis("camera_back", "camera_forward") +
                     right() * input.axis("camera_left", "camera_right") +
                     glm::vec3(0.0f, 1.0f, 0.0f) * input.axis("camera_down", "camera_up");
    if (glm::dot(move, move) > 0.0f) {
        float speed = moveSpeed * (input.isActionDown("camera_fast") ? fastMultiplier : 1.0f);
        position += glm::normalize(move) * speed * static_cast<float>(frameDt);
    }

    // Scroll changes speed, like most editors.
    if (input.scrollDelta() != 0.0f) {
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
