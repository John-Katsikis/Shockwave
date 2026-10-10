#include "Scene.h"

#include <Engine/Components.h>

namespace editor {

using namespace shockwave;

void makeScene(SceneTemplate kind, World& world, const Assets& assets) {
    world.clear();
    const Entity environment = world.create();
    world.add<Name>(environment, {"Environment"});
    world.add<Environment>(environment);
    if (kind == SceneTemplate::Empty) return;

    // The camera the game sees through, framing the origin from above and behind.
    const Entity camera = world.create();
    world.add<Name>(camera, {"Main Camera"});
    world.add<Transform>(camera, {{0.0f, 3.0f, 9.0f}, fromEulerDegrees({-15.0f, 0.0f, 0.0f})});
    world.add<Camera>(camera);

    // A warm sun from above and to the side, so the scene isn't flat.
    const Entity sun = world.create();
    world.add<Name>(sun, {"Sun"});
    world.add<Transform>(sun, {{0.0f, 10.0f, 0.0f}, fromEulerDegrees({-50.0f, 35.0f, 0.0f})});
    Light light;
    light.type = Light::Type::Directional;
    light.color = {1.0f, 0.95f, 0.86f, 1.0f};
    light.intensity = 2.5f;
    world.add<Light>(sun, light);

    // 50 x 50 units with the prototyping grid repeated once per unit.
    const Entity ground = world.create();
    world.add<Name>(ground, {"Ground"});
    world.add<Transform>(ground, {glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), {50.0f, 1.0f, 50.0f}});
    MeshRenderer renderer;
    renderer.mesh = assets.findMesh("plane");
    renderer.texture = assets.findTexture("grid").value_or(TextureHandle{});
    renderer.tiling = {50.0f, 50.0f};
    world.add<MeshRenderer>(ground, renderer);
}

}   // namespace editor
