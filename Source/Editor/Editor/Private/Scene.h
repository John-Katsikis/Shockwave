#pragma once

#include <Assets/Assets.h>
#include <Engine/World.h>

#include <glm/glm.hpp>

namespace editor {

// Where the editor's camera was in a scene, kept per user in the project's .shockwave
// folder (not in the scene file, so moving the view doesn't change what's committed).
struct EditorCamera {
    glm::vec3 position{6.0f, 4.0f, 8.0f};
    float yaw = 0.64f;     // radians, same convention as shockwave::FlyCamera
    float pitch = -0.36f;
};

// What File > New scene starts from.
enum class SceneTemplate {
    Basic,   // sky, a camera and a ground plane: ready to build on and to press Play
    Empty,   // just the sky
};

void makeScene(SceneTemplate kind, shockwave::World& world, const shockwave::Assets& assets);

} // namespace editor
