// The Terrain component and its checks: see Engine/Terrain.h and docs/Terrain.md.
#include <Engine/Terrain.h>

#include <Assets/Assets.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Engine/PhysicsComponents.h>

#include <cmath>

namespace shockwave {

void registerTerrainComponents() {
    static bool done = false;
    if (done) return;
    done = true;
    registerComponent<Terrain>(
        "Terrain",
        {
            field("asset", &Terrain::asset),
            field("visible", &Terrain::visible),
            field("cast_shadows", &Terrain::castShadows),
        },
        "Rendering");
}

std::vector<std::string> terrainProblems(
    const World& world, const Assets& assets, const std::function<std::string(std::string_view)>& whyNotLoaded) {
    std::vector<std::string> problems;
    auto named = [&](Entity e) {
        const Name* n = world.get<Name>(e);
        return "entity '" + (n && !n->value.empty() ? n->value : std::to_string(e.index)) + "'";
    };
    world.forEachEntity([&](Entity e) {
        const Terrain* terrain = world.get<Terrain>(e);
        const Collider* collider = world.get<Collider>(e);
        if (collider && collider->shape == Collider::Shape::Terrain) {
            if (!terrain) problems.push_back(named(e) + ": a Terrain collider needs a Terrain component");
            const RigidBody* body = world.get<RigidBody>(e);
            if (body && body->motion != RigidBody::Motion::Static)
                problems.push_back(named(e) + ": a Terrain collider must be on a Static body");
        }
        if (!terrain) return;
        if (terrain->asset.empty()) {
            problems.push_back(named(e) + ": the Terrain names no .swterrain asset");
            return;
        }
        if (const Transform* t = world.get<Transform>(e)) {
            const glm::mat4 m = worldMatrix(world, e);
            const glm::vec3 x(m[0]), y(m[1]), z(m[2]);
            const float eps = 1e-4f;
            const bool square = std::abs(glm::length(x) - 1) < eps && std::abs(glm::length(y) - 1) < eps &&
                                std::abs(glm::length(z) - 1) < eps;
            const bool upright = std::abs(x.y) < eps && std::abs(x.z) < eps && std::abs(z.x) < eps &&
                                 std::abs(z.y) < eps && x.x > 0 && z.z > 0;
            if (!square || !upright)
                problems.push_back(
                    named(e) +
                    ": a Terrain is never rotated or scaled; set its Transform's rotation to 0 and scale to 1");
            (void)t;
        }
        const auto asset = assets.terrain(terrain->asset);
        if (!asset || !asset->data) {
            const std::string why = whyNotLoaded ? whyNotLoaded(terrain->asset) : std::string();
            problems.push_back(
                named(e) + ": the terrain " + terrain->asset + " isn't loaded" + (why.empty() ? "" : ": " + why));
            return;
        }
        const std::string& material = asset->data->material;
        if (!material.empty() && !assets.findMaterial(material).valid())
            problems.push_back(terrain->asset + ": the material " + material + " isn't loaded");
    });
    return problems;
}

}   // namespace shockwave
