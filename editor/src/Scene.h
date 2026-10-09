#pragma once

#include <shockwave/Assets.h>
#include <shockwave/World.h>

#include <glm/glm.hpp>

namespace editor {

// Where the editor's camera was when the scene was saved, so it reopens there.
struct EditorCamera {
    glm::vec3 position{4.0f, 2.8f, 5.5f};
    float yaw = 0.63f;     // radians, same convention as shockwave::FlyCamera
    float pitch = -0.33f;
};

// The sample scene the editor starts with: a few shapes, two of which move in play mode.
void makeDefaultScene(shockwave::World& world, const shockwave::Assets& assets);
// An environment and a floor.
void makeEmptyScene(shockwave::World& world, const shockwave::Assets& assets);

} // namespace editor
