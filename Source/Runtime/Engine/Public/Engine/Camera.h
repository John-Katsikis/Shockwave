#pragma once

#include <glm/glm.hpp>

namespace shockwave {

class Input;

// Reverse-Z perspective with an infinite far plane: depth is 1 at the near plane and
// falls towards 0 at infinity. Float precision is spread evenly over distance, so there
// is no z-fighting far away and no far plane to tune. Right-handed, Y up, looks down -Z.
glm::mat4 perspectiveReverseZ(float fovYRadians, float aspect, float nearPlane);

// Free-flying camera, like an editor viewport. Hold the right mouse button to look around,
// WASD to move, E/Q up and down, Shift to go faster.
class FlyCamera {
public:
    glm::vec3 position{0.0f, 2.0f, 8.0f};
    float yaw = 0.0f;   // radians, 0 looks down -Z
    float pitch = 0.0f;   // radians, positive looks up
    float fovY = glm::radians(70.0f);
    float nearPlane = 0.05f;
    float moveSpeed = 8.0f;   // units per second
    float fastMultiplier = 4.0f;
    float lookSensitivity = 0.0025f;   // radians per pixel

    // Binds the default actions (camera_forward, camera_back, camera_left, camera_right,
    // camera_up, camera_down, camera_fast, camera_look). Rebind them to change controls.
    static void bindDefaultControls(Input& input);

    // Call once per rendered frame with the real frame time (Application::onFrame).
    void update(Input& input, double frameDt);

    void lookAt(const glm::vec3& target);
    glm::vec3 forward() const;
    glm::vec3 right() const;
    glm::mat4 view() const;
    glm::mat4 projection(float aspect) const;
};

}   // namespace shockwave
