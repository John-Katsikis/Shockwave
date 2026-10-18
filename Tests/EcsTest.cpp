// Entity/component system tests. No GPU or window needed: ctest --test-dir build
#include <Engine/Components.h>
#include <Engine/EntityRef.h>
#include <Engine/Hierarchy.h>
#include <Engine/SceneFile.h>
#include <Engine/Schedule.h>
#include <Engine/Systems.h>
#include <Physics/Physics.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>

using namespace shockwave;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }
bool near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

struct Velocity {
    glm::vec3 value{0.0f};
};
struct Health {
    float current = 100.0f;
    int lives = 3;
    std::string tag = "player";
};

void entities() {
    World w;
    const Entity a = w.create();
    const Entity b = w.create();
    CHECK(w.alive(a) && w.alive(b) && w.entityCount() == 2);
    w.destroy(a);
    CHECK(!w.alive(a) && w.entityCount() == 1);
    w.destroy(a);   // twice is harmless
    CHECK(w.entityCount() == 1);
    CHECK(!w.has<Transform>(a));
    // Creation order is kept even after deletes.
    const Entity c = w.create();
    CHECK(c.index == 2);
    CHECK(w.entityAt(0) == b && w.entityAt(1) == c && w.entityAt(2) == kNullEntity);
}

void recycling() {
    World w;
    std::vector<Entity> all;
    for (int i = 0; i < 3000; ++i) all.push_back(w.create());
    for (Entity e : all) w.destroy(e);
    const Entity fresh = w.create();
    // An old handle to the same slot never matches the new entity.
    for (Entity e : all) {
        if (e.index == fresh.index) CHECK(!w.alive(e));
    }
    CHECK(w.alive(fresh) && w.entityCount() == 1);
}

void components() {
    World w;
    const Entity e = w.create();
    w.add<Transform>(e, {{1, 2, 3}});
    w.add<Velocity>(e, {{0, 1, 0}});
    CHECK(w.has<Transform>(e) && w.has<Velocity>(e) && !w.has<Spin>(e));
    CHECK(w.get<Transform>(e)->position == glm::vec3(1, 2, 3));
    w.add<Transform>(e, {{4, 5, 6}});   // replaces
    CHECK(w.count<Transform>() == 1 && w.get<Transform>(e)->position.x == 4.0f);
    w.remove<Velocity>(e);
    CHECK(!w.has<Velocity>(e) && w.get<Velocity>(e) == nullptr);
    w.destroy(e);
    CHECK(w.count<Transform>() == 0);
}

void iteration() {
    World w;
    for (int i = 0; i < 100; ++i) {
        const Entity e = w.create();
        w.add<Transform>(e, {{float(i), 0, 0}});
        if (i % 2 == 0) w.add<Velocity>(e, {{0, 1, 0}});
    }
    int visited = 0;
    w.each<Transform, const Velocity>([&](Transform& t, const Velocity& v) {
        t.position += v.value;
        ++visited;
    });
    CHECK(visited == 50);
    int moved = 0;
    w.each<const Transform>([&](Entity e, const Transform& t) {
        if (t.position.y == 1.0f) {
            ++moved;
            CHECK(int(t.position.x) % 2 == 0 && w.has<Velocity>(e));
        }
    });
    CHECK(moved == 50);
    // No pool yet for a type: nothing to visit, no crash.
    w.each<Spin>([&](Spin&) { CHECK(false); });
}

void parallel() {
    JobSystem jobs(4);
    World w;
    constexpr int kCount = 100000;
    for (int i = 0; i < kCount; ++i) {
        const Entity e = w.create();
        w.add<Transform>(e);
        w.add<Velocity>(e, {{1, 0, 0}});
    }
    std::atomic<int> visited{0};
    w.parallelEach<Transform, const Velocity>(jobs, [&](Transform& t, const Velocity& v) {
        t.position += v.value;
        visited.fetch_add(1, std::memory_order_relaxed);
    });
    CHECK(visited == kCount);
    bool allMovedOnce = true;
    w.each<const Transform>([&](const Transform& t) { allMovedOnce &= t.position.x == 1.0f; });
    CHECK(allMovedOnce);
}

void copying() {
    World w;
    const Entity e = w.create();
    w.add<Name>(e, {"Crate"});
    w.add<Health>(e);
    World snapshot = w;
    w.get<Name>(e)->value = "Changed";
    w.destroy(e);
    CHECK(!w.alive(e));
    w = snapshot;   // undo: the same handle works again
    CHECK(w.alive(e) && w.get<Name>(e)->value == "Crate" && w.get<Health>(e)->lives == 3);

    const Entity copy = w.clone(e);
    CHECK(copy != e && w.get<Name>(copy)->value == "Crate" && w.has<Health>(copy));
    w.get<Name>(copy)->value = "Copy";
    CHECK(w.get<Name>(e)->value == "Crate");
}

void commands() {
    JobSystem jobs(2);
    World w;
    for (int i = 0; i < 10; ++i) w.add<Transform>(w.create(), {{float(i), 0, 0}});
    Schedule s;
    s.each<const Transform>("cull", [](const SystemContext& ctx, Entity e, const Transform& t) {
        if (t.position.x >= 5.0f) ctx.commands().destroy(e);
    });
    s.add("spawn", {}, [](const SystemContext& ctx) {
        ctx.commands().spawn([](World& world, Entity e) { world.add<Name>(e, {"spawned"}); });
    });
    s.run(w, jobs, 1.0 / 60.0);
    CHECK(w.count<Transform>() == 5);
    CHECK(w.count<Name>() == 1);
}

void scheduling() {
    Schedule s;
    s.each<Transform, const Spin>("Spin", [](const SystemContext&, Transform&, const Spin&) {});
    s.each<Transform, Bob>("Bob", [](const SystemContext&, Transform&, Bob&) {});   // writes Transform too
    s.each<Velocity>("Damping", [](const SystemContext&, Velocity&) {});   // touches neither
    s.each<const Transform, const Velocity>("Read", [](const SystemContext&, const Transform&, const Velocity&) {});
    const auto stages = s.stages();
    CHECK(stages.size() == 3);
    CHECK(stages[0] == std::vector<std::string>({"Spin", "Damping"}));
    CHECK(stages[1] == std::vector<std::string>({"Bob"}));
    CHECK(stages[2] == std::vector<std::string>({"Read"}));

    // Systems in one stage really do run at the same time.
    JobSystem jobs(4);
    World w;
    std::atomic<int> inside{0}, maxInside{0};
    Schedule overlap;
    for (int i = 0; i < 2; ++i) {
        overlap.add("wait" + std::to_string(i), {}, [&](const SystemContext&) {
            const int now = ++inside;
            int seen = maxInside.load();
            while (seen < now && !maxInside.compare_exchange_weak(seen, now)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            --inside;
        });
    }
    overlap.run(w, jobs, 0.0);
    CHECK(maxInside == 2);
}

void builtinSystems() {
    JobSystem jobs(2);
    World w;
    const Entity e = w.create();
    w.add<Transform>(e);
    w.add<Spin>(e, {{0, 1, 0}, 90.0f});
    w.add<Bob>(e, {.amplitude = 1.0f, .frequency = 0.25f});
    Schedule s;
    addBuiltinSystems(s);
    for (int i = 0; i < 60; ++i) {
        storePreviousTransforms(w, jobs);
        s.run(w, jobs, 1.0 / 60.0);
    }
    const Transform& t = *w.get<Transform>(e);
    CHECK(near(eulerDegrees(t.rotation).y, 90.0f, 0.01f));   // 90 degrees per second for one second
    CHECK(near(t.position.y, 1.0f, 1e-3f));   // a quarter cycle of a 1-unit bob
    CHECK(w.has<PreviousTransform>(e));
}

void transforms() {
    Transform t{{1, 2, 3}, fromEulerDegrees({10, 20, 30}), {2, 3, 4}};
    const Transform back = Transform::fromMatrix(t.matrix());
    CHECK(near(back.position, t.position) && near(back.scale, t.scale, 1e-3f));
    CHECK(near(eulerDegrees(back.rotation), {10, 20, 30}, 1e-2f));
    // Same convention as the old editor (ImGuizmo): X first, then Y, then Z.
    const glm::mat4 m = Transform{{}, fromEulerDegrees({90, 0, 0}), glm::vec3(1)}.matrix();
    CHECK(near(glm::vec3(m * glm::vec4(0, 1, 0, 0)), {0, 0, 1}));
    // Turns past 90 degrees about one axis read back as that one angle.
    CHECK(near(eulerDegrees(fromEulerDegrees({0, 120, 0})), {0, 120, 0}, 1e-2f));
    CHECK(near(eulerDegrees(fromEulerDegrees({0, -150, 0})), {0, -150, 0}, 1e-2f));
    CHECK(near(eulerDegrees(fromEulerDegrees({30, 100, 0})), {30, 100, 0}, 1e-2f));
}

void sceneFiles() {
    registerComponent<Health>(
        "Health", {field("current", &Health::current), field("lives", &Health::lives), field("tag", &Health::tag)});
    Assets assets;
    assets.addMesh("cube", MeshHandle{7});
    assets.addTexture("checker", TextureHandle{3});

    World w;
    const Entity env = w.create();
    w.add<Name>(env, {"Environment"});
    w.add<Environment>(env, {{0.1f, 0.2f, 0.3f, 1.0f}});
    const Entity e = w.create();
    w.add<Name>(e, {"Hero \"one\""});
    w.add<Transform>(e, {{1, 2, 3}, fromEulerDegrees({0, 45, 0}), {1, 2, 1}});
    w.add<MeshRenderer>(
        e, {.mesh = MeshHandle{7},
            .texture = TextureHandle{3},
            .tint = {1, 0.5f, 0.25f, 1},
            .tiling = {4, 2.5f},
            .visible = false});
    w.add<Health>(e, {42.0f, 2, "boss"});
    w.add<Velocity>(e);   // not registered: not saved
    w.create();   // bare entity

    std::string error;
    int headerSeen = 0;
    SceneFileHooks hooks;
    hooks.writeHeader = [](std::ostream& out) { out << "editor_camera 1 2 3\n"; };
    hooks.readHeader = [&](const std::string& key, std::istream&) { headerSeen += key == "editor_camera"; };
    CHECK(saveScene(w, assets, "test.swscene", error, hooks));

    World loaded;
    std::vector<std::string> warnings;
    CHECK(loadScene(loaded, assets, "test.swscene", error, hooks, &warnings));
    CHECK(headerSeen == 1 && warnings.empty() && loaded.entityCount() == 3);
    const Entity le = loaded.entityAt(1);
    CHECK(loaded.get<Name>(le)->value == "Hero \"one\"");
    CHECK(near(loaded.get<Transform>(le)->position, {1, 2, 3}));
    CHECK(near(eulerDegrees(loaded.get<Transform>(le)->rotation).y, 45.0f, 1e-3f));
    CHECK(loaded.get<MeshRenderer>(le)->mesh.id == 7 && loaded.get<MeshRenderer>(le)->texture.id == 3);
    CHECK(!loaded.get<MeshRenderer>(le)->visible);
    CHECK(near(loaded.get<MeshRenderer>(le)->tiling.x, 4.0f) && near(loaded.get<MeshRenderer>(le)->tiling.y, 2.5f));
    CHECK(loaded.get<Health>(le)->lives == 2 && loaded.get<Health>(le)->tag == "boss");
    CHECK(!loaded.has<Velocity>(le));
    CHECK(near(loaded.get<Environment>(loaded.entityAt(0))->skyColor.g, 0.2f));
    CHECK(!loaded.has<Name>(loaded.entityAt(2)));

    // Version 1 files from the first editor still open.
    {
        std::ofstream out("v1.swscene");
        out << "shockwave_scene 1\nsky_color 0.5 0.6 0.7\ncamera 4 2.8 5.5 0.63 -0.33\n\n"
               "entity \"Crate\"\n  mesh cube\n  texture checker\n  position 0 0.5 0\n  rotation 0 20 0\n"
               "  scale 1 1 1\n  tint 1 0.62 0.25 1\n  visible 1\nend\n"
               "entity \"Odd\"\n  mesh teapot\n  texture white\nend\n";
    }
    int cameraSeen = 0;
    SceneFileHooks v1hooks;
    v1hooks.readHeader = [&](const std::string& key, std::istream&) { cameraSeen += key == "camera"; };
    CHECK(loadScene(loaded, assets, "v1.swscene", error, v1hooks, &warnings));
    CHECK(cameraSeen == 1 && loaded.entityCount() == 3 && warnings.size() == 1);
    CHECK(near(loaded.get<Environment>(loaded.entityAt(0))->skyColor.b, 0.7f));
    const Entity crate = loaded.entityAt(1);
    CHECK(loaded.get<Name>(crate)->value == "Crate");
    CHECK(near(eulerDegrees(loaded.get<Transform>(crate)->rotation).y, 20.0f, 1e-3f));
    CHECK(loaded.get<MeshRenderer>(crate)->mesh.id == 7);

    // Broken files fail with a line number and leave the world alone.
    {
        std::ofstream out("bad.swscene");
        out << "shockwave_scene 2\nentity \"x\"\n  Transform\n    position 1 two 3\nend\n";
    }
    World untouched = loaded;
    CHECK(!loadScene(untouched, assets, "bad.swscene", error));
    CHECK(error.find(":4:") != std::string::npos && untouched.entityCount() == 3);
}

void hierarchy() {
    World w;
    const Entity root = w.create();
    w.add<Transform>(root, {{10, 0, 0}, fromEulerDegrees({0, 90, 0}), glm::vec3(2)});
    const Entity child = w.create();
    w.add<Transform>(child, {{1, 0, 0}});
    const Entity grandchild = w.create();
    w.add<Transform>(grandchild, {{0, 1, 0}});
    const Entity loose = w.create();
    w.add<Transform>(loose, {{5, 5, 5}});

    // Attaching without keeping the world position: the local transform is untouched.
    CHECK(setParent(w, child, root, false));
    CHECK(near(w.get<Transform>(child)->position, {1, 0, 0}));
    // root turns 90 degrees about Y and doubles: local +X lands at world -Z, two units out.
    CHECK(near(glm::vec3(worldMatrix(w, child)[3]), {10, 0, -2}));
    CHECK(setParent(w, grandchild, child, false));
    CHECK(near(glm::vec3(worldMatrix(w, grandchild)[3]), {10, 2, -2}));

    CHECK(parentOf(w, grandchild) == child && parentOf(w, root) == kNullEntity);
    CHECK(isAncestor(w, root, grandchild) && !isAncestor(w, grandchild, root));
    CHECK(childrenOf(w, root) == std::vector<Entity>{child});
    CHECK((subtree(w, root) == std::vector<Entity>{root, child, grandchild}));

    // Loops are refused.
    CHECK(!setParent(w, root, grandchild));
    CHECK(!setParent(w, root, root));
    CHECK(parentOf(w, root) == kNullEntity);

    // Keeping the world position: it stays put and the local transform absorbs the parent.
    CHECK(setParent(w, loose, child));
    CHECK(near(glm::vec3(worldMatrix(w, loose)[3]), {5, 5, 5}, 1e-3f));
    CHECK(near(w.get<Transform>(loose)->scale, glm::vec3(0.5f), 1e-4f));
    CHECK(setParent(w, loose, kNullEntity));
    CHECK(!w.has<Parent>(loose) && near(w.get<Transform>(loose)->position, {5, 5, 5}, 1e-3f));

    // Cloning a branch: the copies hang off each other, the copy of the top keeps its parent.
    const Entity copy = cloneWithChildren(w, child);
    CHECK(w.entityCount() == 6 && parentOf(w, copy) == root);
    const std::vector<Entity> copied = subtree(w, copy);
    CHECK(copied.size() == 2 && copied[1] != grandchild && parentOf(w, copied[1]) == copy);
    CHECK(near(glm::vec3(worldMatrix(w, copied[1])[3]), {10, 2, -2}));

    // A destroyed parent leaves children at the top level; destroyWithChildren takes them too.
    w.destroy(copy);
    CHECK(parentOf(w, copied[1]) == kNullEntity && near(glm::vec3(worldMatrix(w, copied[1])[3]), {0, 1, 0}));
    destroyWithChildren(w, root);
    CHECK(!w.alive(root) && !w.alive(child) && !w.alive(grandchild) && w.alive(loose) && w.entityCount() == 2);
}

void sceneParents() {
    Assets assets;
    World w;
    const Entity child = w.create();   // the child's block comes first: links can point down the file
    w.add<Name>(child, {"Wheel"});
    w.add<Transform>(child, {{1, 0, 0}});
    const Entity parent = w.create();
    w.add<Name>(parent, {"Car"});
    w.add<Transform>(parent, {{0, 0, 4}});
    setParent(w, child, parent, false);
    const Entity gone = w.create();
    const Entity orphan = w.create();
    w.add<Parent>(orphan, {gone});
    w.destroy(gone);   // a dangling link saves as none

    std::string error;
    CHECK(saveScene(w, assets, "parents.swscene", error));
    World loaded;
    std::vector<std::string> warnings;
    CHECK(loadScene(loaded, assets, "parents.swscene", error, {}, &warnings));
    CHECK(warnings.empty() && loaded.entityCount() == 3);
    const Entity wheel = loaded.entityAt(0), car = loaded.entityAt(1);
    CHECK(parentOf(loaded, wheel) == car && near(glm::vec3(worldMatrix(loaded, wheel)[3]), {1, 0, 4}));
    CHECK(loaded.get<Parent>(loaded.entityAt(2))->entity == kNullEntity);

    // A link to a block that isn't there loads empty, with a warning.
    {
        std::ofstream out("badlink.swscene");
        out << "shockwave_scene 2\nentity \"x\"\n  Parent\n    entity 9\nend\n";
    }
    CHECK(loadScene(loaded, assets, "badlink.swscene", error, {}, &warnings));
    CHECK(warnings.size() == 1 && loaded.get<Parent>(loaded.entityAt(0))->entity == kNullEntity);

    // Asset names with spaces are quoted, and read back.
    assets.addTexture("my bricks.png", TextureHandle{4});
    World named;
    const Entity e = named.create();
    named.add<MeshRenderer>(e, {MeshHandle{}, TextureHandle{4}});
    CHECK(saveScene(named, assets, "names.swscene", error));
    CHECK(loadScene(loaded, assets, "names.swscene", error));
    CHECK(loaded.get<MeshRenderer>(loaded.entityAt(0))->texture.id == 4);
}

// Scenes written by hand or by tools: names instead of numbers, and loud mistakes.
void authoredScenes() {
    Assets assets;
    std::string error;
    std::vector<std::string> warnings;
    {
        std::ofstream out("authored.swscene");
        out << "shockwave_scene 2\n"
               "entity \"Wheel\"\n  Parent\n    entity \"Car\"\n  Collider\n    shape \"Convex model\"\n"
               "  RigidBody\n    motion dynamic\n    sensor true\nend\n"
               "entity \"Car\"\n  Transform\n    position 0 0 4\nend\n";
    }
    World loaded;
    CHECK(loadScene(loaded, assets, "authored.swscene", error, {}, &warnings));
    CHECK(warnings.empty());
    const Entity wheel = loaded.entityAt(0), car = loaded.entityAt(1);
    CHECK(parentOf(loaded, wheel) == car);
    CHECK(loaded.get<Collider>(wheel)->shape == Collider::Shape::ConvexModel);
    CHECK(loaded.get<RigidBody>(wheel)->sensor);

    // Saving writes unique names and option names, so the file reads the same way.
    CHECK(saveScene(loaded, assets, "authored.swscene", error));
    {
        std::ifstream in("authored.swscene");
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text.find("entity \"Car\"\n  Transform") != std::string::npos);
        CHECK(text.find("    entity \"Car\"\n") != std::string::npos);
        CHECK(text.find("shape \"Convex model\"") != std::string::npos);
    }

    // Typos, unknown names, ambiguous links and out-of-range values all warn with a line.
    {
        std::ofstream out("mistakes.swscene");
        out << "shockwave_scene 2\n"
               "entity \"A\"\n  Transform\n    positon 1 2 3\n    scale 0 1 1\n  Parent\n    entity \"B\"\nend\n"
               "entity \"B\"\nend\nentity \"B\"\n  Parent\n    entity \"Nobody\"\n  Wobble\n    speed 2\nend\n";
    }
    CHECK(loadScene(loaded, assets, "mistakes.swscene", error, {}, &warnings));
    CHECK(warnings.size() == 5);
    bool lineNumbers = true;
    for (const std::string& w : warnings) lineNumbers &= w.rfind("mistakes.swscene:", 0) == 0;
    CHECK(lineNumbers);
    CHECK(loaded.get<Parent>(loaded.entityAt(0))->entity == kNullEntity);

    // A bad enum fails and says what it accepts.
    {
        std::ofstream out("badenum.swscene");
        out << "shockwave_scene 2\nentity\n  Collider\n    shape Cone\nend\n";
    }
    CHECK(!loadScene(loaded, assets, "badenum.swscene", error));
    CHECK(error.find("Sphere") != std::string::npos);

    // The schema lists every component and the asset names a scene can use.
    assets.addMesh("cube", MeshHandle{1});
    std::ostringstream schema;
    writeSceneSchema(schema, assets);
    CHECK(schema.str().find("#### Collider") != std::string::npos);
    CHECK(schema.str().find("\"Convex model\"") != std::string::npos);
    CHECK(schema.str().find("- `cube`") != std::string::npos);
}

// Lights, materials and the environment's look are plain fields an author or a tool
// writes by name; they come back exactly.
void lighting() {
    Assets assets;
    std::string error;
    std::vector<std::string> warnings;
    {
        std::ofstream out("lit.swscene");
        out << "shockwave_scene 2\n"
               "entity \"Environment\"\n  Environment\n    ground_color 0.1 0.1 0.1\n    exposure -1.5\n"
               "    tonemapper AgX\n    environment_intensity 2.5\n    environment_rotation 90\n"
               "    show_environment no\n    bloom_intensity 0.1\nend\n"
               "entity \"Spot\"\n  Transform\n    rotation -90 0 0\n  Light\n    type Spot\n    color 1 0.5 0.25\n"
               "    intensity 12\n    range 8\n    spot_angle 25\n    spot_softness 0.5\n    enabled no\nend\n"
               "entity \"Glow\"\n  MeshRenderer\n    roughness 0.2\n    metallic 1\n    emissive 0 1 0\n"
               "    emissive_intensity 4\nend\n";
    }
    World loaded;
    CHECK(loadScene(loaded, assets, "lit.swscene", error, {}, &warnings));
    CHECK(warnings.empty());
    for (int pass = 0; pass < 2; ++pass) {
        const Environment* env = loaded.get<Environment>(loaded.entityAt(0));
        const Light* light = loaded.get<Light>(loaded.entityAt(1));
        const MeshRenderer* glow = loaded.get<MeshRenderer>(loaded.entityAt(2));
        CHECK(env && light && glow);
        if (!env || !light || !glow) return;
        CHECK(near(env->exposure, -1.5f) && env->tonemapper == Environment::Tonemapper::AgX);
        CHECK(near(env->groundColor.r, 0.1f));
        CHECK(env->environmentMap.id == 0 && near(env->environmentIntensity, 2.5f));
        CHECK(near(env->environmentRotation, 90.0f) && !env->showEnvironment);
        CHECK(near(env->bloomIntensity, 0.1f));
        CHECK(light->type == Light::Type::Spot && !light->enabled);
        CHECK(near(light->intensity, 12.0f) && near(light->range, 8.0f) && near(light->spotAngle, 25.0f));
        CHECK(near(light->spotSoftness, 0.5f) && near(light->color.g, 0.5f));
        CHECK(near(glow->roughness, 0.2f) && near(glow->metallic, 1.0f));
        CHECK(near(glow->emissive.g, 1.0f) && near(glow->emissiveIntensity, 4.0f));
        // Save and read it back: the second pass checks the round trip.
        CHECK(saveScene(loaded, assets, "lit.swscene", error));
        CHECK(loadScene(loaded, assets, "lit.swscene", error, {}, &warnings));
        CHECK(warnings.empty());
    }
    {
        std::ifstream in("lit.swscene");
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text.find("type Spot") != std::string::npos);
        CHECK(text.find("tonemapper AgX") != std::string::npos);
    }

    // Out-of-range light values warn rather than silently clamping.
    {
        std::ofstream out("badlight.swscene");
        out << "shockwave_scene 2\nentity\n  Light\n    spot_angle 120\n    range -1\nend\n";
    }
    CHECK(loadScene(loaded, assets, "badlight.swscene", error, {}, &warnings));
    CHECK(warnings.size() == 2);
    std::ostringstream schema;
    writeSceneSchema(schema, assets);
    CHECK(schema.str().find("#### Light") != std::string::npos);
    CHECK(schema.str().find("`tonemapper`") != std::string::npos);
}

}   // namespace

// Cameras: which one a game looks through, and the matrices it gets.
void cameras() {
    World w;
    CHECK(!primaryCamera(w).valid());
    const Entity a = w.create();
    w.add<Transform>(a);
    w.add<Camera>(a, {.fov = 50.0f, .nearPlane = 0.1f, .primary = false});
    CHECK(primaryCamera(w) == a);   // no primary one: the first camera
    const Entity rig = w.create();
    w.add<Transform>(rig, {{0, 0, 10}, fromEulerDegrees({0, 90, 0}), {3, 3, 3}});
    const Entity b = w.create();
    w.add<Transform>(b, {{0, 2, 0}});
    w.add<Camera>(b);
    w.add<Parent>(b, {rig});
    CHECK(primaryCamera(w) == b);

    glm::mat4 view, projection;
    CHECK(cameraMatrices(w, b, 16.0f / 9.0f, view, projection));
    // Scale on the rig moves the camera (2 * 3 = 6 up) but doesn't scale the view.
    const glm::mat4 inv = glm::inverse(view);
    CHECK(near(glm::vec3(inv[3]), {0, 6, 10}, 1e-3f));
    CHECK(near(glm::length(glm::vec3(view[0])), 1.0f, 1e-4f));
    // Turned 90 degrees about Y, so it looks down -X.
    CHECK(near(-glm::vec3(inv[2]), {-1, 0, 0}, 1e-4f));
    CHECK(!cameraMatrices(w, rig, 1.0f, view, projection));
}

// EntityRef and findEntity: what game code uses to work with one named entity.
void entityRefs() {
    World w;
    const Entity crate = w.create();
    w.add(crate, Name{"Crate"});
    w.add(crate, Transform{});
    const Entity child = w.create();
    w.add(child, Parent{crate});

    CHECK(!findEntity(w, "Nothing"));
    EntityRef ref = findEntity(w, "Crate");
    CHECK(ref && ref.entity() == crate);
    CHECK(ref.has<Transform>() && !ref.has<Spin>() && ref.find<Spin>() == nullptr);
    ref.add(Spin{.speed = 2.0f});
    CHECK(ref.get<Spin>().speed == 2.0f);
    ref.remove<Spin>();
    CHECK(!ref.has<Spin>());
    ref.destroy();
    CHECK(!ref && !w.alive(child) && w.entityCount() == 0);
    CHECK(!EntityRef() && !EntityRef().has<Transform>());
}

int main() {
    entities();
    recycling();
    components();
    iteration();
    parallel();
    copying();
    commands();
    scheduling();
    builtinSystems();
    transforms();
    sceneFiles();
    hierarchy();
    sceneParents();
    authoredScenes();
    lighting();
    cameras();
    entityRefs();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("All ECS tests passed\n");
    return 0;
}
