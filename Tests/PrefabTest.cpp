// Prefab tests: loading, copies in scenes, nesting, what gets saved, hot reload, spawning,
// applying and unpacking. No GPU or window needed: ctest --test-dir build
#include <Assets/Assets.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Engine/Prefab.h>
#include <Engine/SceneFile.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace shockwave;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

bool near(float a, float b) { return std::abs(a - b) <= 1e-4f; }

const fs::path kContent = fs::absolute("PrefabContent");

void write(const std::string& path, const std::string& text) {
    const fs::path file = kContent / path;
    fs::create_directories(file.parent_path());
    std::ofstream(file) << text;
}

const char* kLamp = R"(shockwave_prefab 1

entity "Lamp"
  Transform
    position 0 0 0
  MeshRenderer
    tint 1 0 0 1
end

entity "Bulb"
  Parent
    entity "Lamp"
  Transform
    position 0 2 0
  Light
    intensity 3
end
)";

const char* kRow = R"(shockwave_prefab 1

entity "Row"
  Transform
end

entity "Left"
  Prefab
    path "Prefabs/Lamp.swprefab"
  Transform
    position -1 0 0
end

entity "Right"
  Prefab
    path "Prefabs/Lamp.swprefab"
  Transform
    position 1 0 0
  MeshRenderer
    tint 0 0 1 1
end
)";

const char* kScene = R"(shockwave_scene 2

entity "Plain lamp"
  Prefab
    path "Prefabs/Lamp.swprefab"
  Transform
    position 5 0 0
end

entity "Green lamp"
  Prefab
    path "Prefabs/Lamp.swprefab"
  Transform
    position 0 0 5
  MeshRenderer
    tint 0 1 0 1
end

entity "Sign"
  Parent
    entity "Green lamp"
  Transform
    position 0 3 0
end

entity "Street"
  Prefab
    path "Prefabs/Row.swprefab"
end
)";

Entity named(const World& world, const std::string& name) {
    Entity found;
    world.forEachEntity([&](Entity e) {
        const Name* n = world.get<Name>(e);
        if (n && n->value == name && !found.valid()) found = e;
    });
    return found;
}

// The part called name under root.
Entity partNamed(const World& world, Entity root, const std::string& name) {
    for (Entity e : subtree(world, root)) {
        const Name* n = world.get<Name>(e);
        if (!(e == root) && n && n->value == name) return e;
    }
    return kNullEntity;
}

bool load(World& world, const Assets& assets, const std::string& text, std::vector<std::string>* warnings = nullptr) {
    std::istringstream in(text);
    std::string error;
    std::vector<std::string> ignored;
    const bool ok = readScene(world, assets, in, "test.swscene", error, {}, warnings ? warnings : &ignored);
    if (!ok) std::printf("load failed: %s\n", error.c_str());
    return ok;
}

std::string save(const World& world, const Assets& assets, const SceneFileHooks& hooks = {}) {
    std::ostringstream out;
    writeScene(out, world, assets, hooks);
    return out.str();
}

void copiesInScenes(const Assets& assets) {
    World world;
    std::vector<std::string> warnings;
    CHECK(load(world, assets, kScene, &warnings));
    for (const std::string& w : warnings) std::printf("unexpected warning: %s\n", w.c_str());
    CHECK(warnings.empty());
    // 2 lamps (root + bulb) + sign + row (root + 2 lamps + 2 bulbs).
    CHECK(world.entityCount() == 2 * 2 + 1 + 5);

    const Entity plain = named(world, "Plain lamp");
    const Entity green = named(world, "Green lamp");
    CHECK(world.has<PrefabInstance>(plain) && !world.has<PrefabPart>(plain));
    CHECK(near(world.get<MeshRenderer>(plain)->tint.r, 1.0f));
    CHECK(near(world.get<MeshRenderer>(green)->tint.g, 1.0f) && near(world.get<MeshRenderer>(green)->tint.r, 0.0f));
    CHECK(near(world.get<Transform>(plain)->position.x, 5.0f));

    const Entity bulb = partNamed(world, plain, "Bulb");
    CHECK(bulb.valid() && world.has<PrefabPart>(bulb));
    CHECK(parentOf(world, bulb) == plain);
    CHECK(near(world.get<Light>(bulb)->intensity, 3.0f));
    CHECK(prefabRootOf(world, bulb) == plain);

    // Something added under a copy stays the scene's own.
    const Entity sign = named(world, "Sign");
    CHECK(parentOf(world, sign) == green && !world.has<PrefabPart>(sign));

    // Nesting: the row's lamps are parts of the row and copies of the lamp.
    const Entity street = named(world, "Street");
    const Entity right = partNamed(world, street, "Right");
    CHECK(right.valid() && world.has<PrefabPart>(right) && world.has<PrefabInstance>(right));
    CHECK(near(world.get<MeshRenderer>(right)->tint.b, 1.0f));
    CHECK(prefabRootOf(world, right) == street);
    CHECK(prefabOwnerOf(world, right) == street);
    const Entity rightBulb = partNamed(world, right, "Bulb");
    CHECK(rightBulb.valid() && prefabOwnerOf(world, rightBulb) == right && prefabRootOf(world, rightBulb) == street);

    // Saving writes only the copies' own changes, and loading that gives the same scene.
    const std::string text = save(world, assets);
    CHECK(text.find("Bulb") == std::string::npos);
    CHECK(text.find("intensity") == std::string::npos);
    CHECK(text.find("tint 0 1 0 1") != std::string::npos);
    CHECK(text.find("tint 1 0 0 1") == std::string::npos);
    CHECK(text.find("\"Sign\"") != std::string::npos);
    World again;
    CHECK(load(again, assets, text));
    CHECK(again.entityCount() == world.entityCount());
    CHECK(save(again, assets) == text);
    CHECK(parentOf(again, named(again, "Sign")) == named(again, "Green lamp"));
}

void hotReload(const Assets& assets) {
    World world;
    CHECK(load(world, assets, kScene));
    CHECK(updatePrefabInstances(world) == 0);   // nothing changed

    write(
        "Prefabs/Lamp.swprefab", std::string(kLamp)
                                     .replace(std::string(kLamp).find("intensity 3"), 11, "intensity 7")
                                     .replace(std::string(kLamp).find("tint 1 0 0 1"), 12, "tint 1 1 0 1"));
    const auto reloaded = prefabs().reload("Prefabs/Lamp.swprefab");
    CHECK(reloaded.size() == 2);   // the lamp, and the row that uses it
    CHECK(updatePrefabInstances(world) == 3);   // two lamps and the street; the row's lamps come with it

    const Entity plain = named(world, "Plain lamp");
    const Entity green = named(world, "Green lamp");
    CHECK(near(world.get<Light>(partNamed(world, plain, "Bulb"))->intensity, 7.0f));
    CHECK(near(world.get<MeshRenderer>(plain)->tint.g, 1.0f));   // followed the prefab
    CHECK(near(world.get<MeshRenderer>(green)->tint.r, 0.0f));   // kept its own
    CHECK(near(world.get<Transform>(green)->position.z, 5.0f));
    CHECK(parentOf(world, named(world, "Sign")) == green);
    const Entity street = named(world, "Street");
    const Entity left = partNamed(world, street, "Left");
    CHECK(near(world.get<Light>(partNamed(world, left, "Bulb"))->intensity, 7.0f));
    CHECK(near(world.get<MeshRenderer>(partNamed(world, street, "Right"))->tint.b, 1.0f));
    CHECK(world.entityCount() == 10);
    write("Prefabs/Lamp.swprefab", kLamp);
    prefabs().reload("Prefabs/Lamp.swprefab");
}

void spawning(const Assets& assets) {
    (void)assets;
    World world;
    Transform at;
    at.position = {1, 2, 3};
    const Entity a = spawnPrefab(world, "Prefabs/Row.swprefab", &at);
    CHECK(a.valid() && world.entityCount() == 5);
    CHECK(near(world.get<Transform>(a)->position.y, 2.0f));
    const Entity b = spawnPrefab(world, "Prefabs/Lamp.swprefab");
    CHECK(near(world.get<Transform>(b)->position.x, 0.0f));
    CHECK(!spawnPrefab(world, "Prefabs/Missing.swprefab").valid());
    CHECK(world.entityCount() == 7);
}

void problems(const Assets& assets) {
    write(
        "Prefabs/A.swprefab", "shockwave_prefab 1\nentity \"A\"\n  Transform\nend\nentity \"B copy\"\n  Prefab\n"
                              "    path \"Prefabs/B.swprefab\"\nend\n");
    write(
        "Prefabs/B.swprefab", "shockwave_prefab 1\nentity \"B\"\n  Transform\nend\nentity \"A copy\"\n  Prefab\n"
                              "    path \"Prefabs/A.swprefab\"\nend\n");
    World world;
    std::vector<std::string> warnings;
    CHECK(load(
        world, assets,
        "shockwave_scene 2\nentity \"Loop\"\n  Prefab\n    path \"Prefabs/A.swprefab\"\nend\n"
        "entity \"Gone\"\n  Prefab\n    path \"Prefabs/Missing.swprefab\"\nend\n",
        &warnings));
    // A loads B, whose copy of A is left out with a warning.
    const auto a = prefabs().load("Prefabs/A.swprefab");
    const auto b = prefabs().load("Prefabs/B.swprefab");
    CHECK(a && b);
    bool loopWarned = false;
    for (const std::string& w : b ? b->warnings : std::vector<std::string>{})
        loopWarned |= w.find("Prefabs/B.swprefab:5: prefab \"Prefabs/A.swprefab\": it contains itself") == 0;
    CHECK(loopWarned);
    CHECK(world.entityCount() == 4);   // Loop, its B copy, B's root... and Gone
    bool missingWarned = false;
    for (const std::string& w : warnings) missingWarned |= w.find("test.swscene:6: prefab \"Prefabs/Missing") == 0;
    CHECK(missingWarned);
}

void exactRoundTrip(const Assets& assets) {
    World world;
    CHECK(load(world, assets, kScene));
    SceneFileHooks exact;
    exact.exact = true;
    const std::string text = save(world, assets, exact);
    CHECK(text.find("Bulb") != std::string::npos && text.find("PrefabPart") != std::string::npos);
    World handles = world.withoutComponents();
    exact.entities = &handles;
    World again;
    std::istringstream in(text);
    std::string error;
    CHECK(readScene(again, assets, in, "exact", error, exact));
    CHECK(again.entityCount() == world.entityCount() && again.count<PrefabPart>() == world.count<PrefabPart>());
    CHECK(updatePrefabInstances(again) == 0);
    CHECK(save(again, assets) == save(world, assets));
}

void authoring(const Assets& assets) {
    // Make a prefab from ordinary entities, place it, apply a change, unpack a copy.
    World world;
    const Entity crate = world.create();
    world.add<Name>(crate, {"Crate"});
    world.add<Transform>(crate, {glm::vec3(4, 0, 0)});
    world.add<MeshRenderer>(crate).tint = {0.5f, 0.5f, 0.5f, 1};
    const Entity lid = world.create();
    world.add<Name>(lid, {"Lid"});
    world.add<Transform>(lid, {glm::vec3(0, 1, 0)});
    world.add<Parent>(lid, {crate});
    std::string error;
    CHECK(savePrefab(world, crate, assets, kContent / "Prefabs/Crate.swprefab", error));
    const auto prefab = prefabs().load("Prefabs/Crate.swprefab", &error);
    CHECK(prefab && prefab->world.entityCount() == 2);
    CHECK(prefab && near(prefab->world.get<Transform>(prefab->root)->position.x, 0.0f));

    World scene;
    Transform at;
    at.position = {2, 0, 0};
    const Entity one = spawnPrefab(scene, "Prefabs/Crate.swprefab", &at);
    const Entity two = spawnPrefab(scene, "Prefabs/Crate.swprefab");
    scene.get<MeshRenderer>(one)->tint = {1, 0, 0, 1};
    const Entity light = scene.create();
    scene.add<Name>(light, {"Lamp"});
    scene.add<Parent>(light, {one});
    CHECK(applyPrefabInstance(scene, one, assets, error));
    CHECK(updatePrefabInstances(scene) == 2);
    CHECK(near(scene.get<MeshRenderer>(two)->tint.r, 1.0f) && near(scene.get<MeshRenderer>(two)->tint.g, 0.0f));
    CHECK(partNamed(scene, two, "Lamp").valid());   // added under one, now in every copy
    CHECK(near(scene.get<Transform>(one)->position.x, 2.0f));   // placement isn't applied

    // Variant: a prefab whose root is a copy of another.
    CHECK(savePrefab(scene, two, assets, kContent / "Prefabs/Red crate.swprefab", error));
    const Entity variant = spawnPrefab(scene, "Prefabs/Red crate.swprefab");
    CHECK(variant.valid() && partNamed(scene, variant, "Lid").valid());
    CHECK(near(scene.get<MeshRenderer>(variant)->tint.r, 1.0f));

    unpackPrefab(scene, two);
    CHECK(!scene.has<PrefabInstance>(two) && !scene.has<PrefabPart>(partNamed(scene, two, "Lid")));
    const std::string text = save(scene, assets);
    World again;
    CHECK(load(again, assets, text));
    CHECK(again.entityCount() == scene.entityCount());
}

}   // namespace

int main() {
    fs::remove_all(kContent);
    write("Prefabs/Lamp.swprefab", kLamp);
    write("Prefabs/Row.swprefab", kRow);
    Assets assets;
    prefabs().setup(kContent, &assets);

    copiesInScenes(assets);
    hotReload(assets);
    spawning(assets);
    problems(assets);
    exactRoundTrip(assets);
    authoring(assets);
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("All prefab tests passed\n");
    return 0;
}
