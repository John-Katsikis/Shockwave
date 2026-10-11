// GameContext::spawn: prefab copies from game code (Engine/Prefab.h).
#include <GameFramework/GameMode.h>

#include <Engine/Components.h>
#include <Engine/Prefab.h>

namespace shockwave {

EntityRef GameContext::spawn(std::string_view prefab) const {
    const Entity root = spawnPrefab(world, prefab);
    return root.valid() ? EntityRef(world, root) : EntityRef();
}

EntityRef GameContext::spawn(std::string_view prefab, const glm::vec3& position) const {
    const Entity root = spawnPrefab(world, prefab);
    if (!root.valid()) return {};
    if (Transform* t = world.get<Transform>(root)) {
        t->position = position;
    } else {
        world.add<Transform>(root, {position});
    }
    return EntityRef(world, root);
}

}   // namespace shockwave
