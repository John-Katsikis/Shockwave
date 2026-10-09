#include "Scene.h"

#include <shockwave/Components.h>
#include <shockwave/Hierarchy.h>

namespace editor {

using namespace shockwave;

namespace {

Entity addShape(World& world, const Assets& assets, const char* name, const char* mesh, const char* texture,
                glm::vec3 position, glm::vec3 scale, glm::vec4 tint, glm::vec3 rotation = glm::vec3(0.0f)) {
    const Entity e = world.create();
    world.add<Name>(e, {name});
    world.add<Transform>(e, {position, fromEulerDegrees(rotation), scale});
    world.add<MeshRenderer>(e, {assets.findMesh(mesh), assets.findTexture(texture).value_or(TextureHandle{}), tint});
    return e;
}

void addEnvironment(World& world) {
    const Entity e = world.create();
    world.add<Name>(e, {"Environment"});
    world.add<Environment>(e);
}

} // namespace

void makeDefaultScene(World& world, const Assets& assets) {
    world.clear();
    addEnvironment(world);
    addShape(world, assets, "Floor", "floor", "grid", {0, 0, 0}, {40, 1, 40}, {1, 1, 1, 1});
    addShape(world, assets, "Crate", "cube", "panel", {0, 0.5f, 0}, {1, 1, 1}, {1.0f, 0.62f, 0.25f, 1}, {0, 20, 0});
    const Entity ball = addShape(world, assets, "Ball", "sphere", "checker", {2.2f, 0.75f, 0.4f}, {1.5f, 1.5f, 1.5f},
                                 {0.35f, 0.65f, 1.0f, 1});
    world.add<Bob>(ball, {.amplitude = 0.4f, .frequency = 0.5f});
    // Textures from the project's assets/ folder, imported by the editor.
    addShape(world, assets, "Pillar", "cube", "textures/bricks.png", {-2.5f, 1.5f, -1.5f}, {1, 3, 1}, {1, 1, 1, 1});
    addShape(world, assets, "Ramp", "cube", "checker", {-0.5f, 0.4f, 2.6f}, {3, 0.2f, 1.6f}, {0.5f, 0.9f, 0.55f, 1},
             {12, -15, 0});
    const Entity small = addShape(world, assets, "Small crate", "cube", "panel", {0.9f, 0.3f, -1.4f},
                                  {0.6f, 0.6f, 0.6f}, {0.95f, 0.35f, 0.4f, 1}, {0, -35, 0});
    world.add<Spin>(small, {{0, 1, 0}, 60.0f});
    // A child: it rides along as the crate spins. Its Transform is in the crate's space,
    // so the crate's 0.6 scale shrinks the offset and the size too.
    const Entity moon = addShape(world, assets, "Moon", "sphere", "checker", {1.8f, 0.4f, 0}, {0.5f, 0.5f, 0.5f},
                                 {0.9f, 0.9f, 1.0f, 1});
    setParent(world, moon, small, false);
    // An empty parent grouping a little stack: move "Stack" and both crates follow.
    const Entity stack = world.create();
    world.add<Name>(stack, {"Stack"});
    world.add<Transform>(stack, {{2.6f, 0, -2.4f}, fromEulerDegrees({0, 25, 0}), glm::vec3(1)});
    const Entity lower = addShape(world, assets, "Lower crate", "cube", "textures/wood.png", {0, 0.4f, 0},
                                  {0.8f, 0.8f, 0.8f}, {1, 1, 1, 1});
    const Entity upper = addShape(world, assets, "Upper crate", "cube", "textures/wood.png", {0.1f, 1.05f, 0},
                                  {0.5f, 0.5f, 0.5f}, {1, 1, 1, 1}, {0, 30, 0});
    setParent(world, lower, stack, false);
    setParent(world, upper, stack, false);
}

void makeEmptyScene(World& world, const Assets& assets) {
    world.clear();
    addEnvironment(world);
    addShape(world, assets, "Floor", "floor", "grid", {0, 0, 0}, {40, 1, 40}, {1, 1, 1, 1});
}

} // namespace editor
