// Navigation without a GPU: baking a mesh from colliders, paths round a wall, incremental
// rebakes, saving and loading, input hashes, settings checks, and NavAgents walking
// during a PlaySession, alone and as a crowd.
#include <Navigation/Navigation.h>

#include <Core/Jobs.h>
#include <Engine/Components.h>
#include <GameFramework/GameInstance.h>
#include <GameFramework/PlaySession.h>

#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace shockwave;
namespace fs = std::filesystem;

namespace {

void check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

Entity box(World& w, const glm::vec3& position, const glm::vec3& size) {
    const Entity e = w.create();
    Transform t{position};
    t.scale = size;
    w.add<Transform>(e, t);
    w.add<Collider>(e, {});   // a unit box, scaled
    return e;
}

// A 20 x 20 floor with a wall across the middle that leaves a gap at x > 6.
struct Level {
    World world;
    Entity floor, wall;
    Level() {
        floor = box(world, {0, -0.25f, 0}, {20, 0.5f, 20});
        wall = box(world, {-2, 1, 0}, {16, 2, 0.5f});
        const Entity settings = world.create();
        NavMeshSettings s;
        s.tileSize = 32;
        world.add<NavMeshSettings>(settings, s);
    }
};

std::shared_ptr<const NavMesh> bake(const World& world, JobSystem& jobs, const NavMesh* previous = nullptr) {
    const NavMeshSettings settings = findNavMeshSettings(world);
    std::string error;
    auto mesh = bakeNavMesh(collectNavGeometry(world, nullptr, settings), settings, jobs, error, previous);
    check(mesh != nullptr, "bake: " + error);
    return mesh;
}

float pathLength(const std::vector<glm::vec3>& points) {
    float length = 0;
    for (size_t i = 1; i < points.size(); ++i) length += glm::length(points[i] - points[i - 1]);
    return length;
}

void bakingAndPaths(JobSystem& jobs) {
    Level level;
    const auto mesh = bake(level.world, jobs);
    const NavMeshStats& s = mesh->stats();
    check(!mesh->empty() && s.tiles > 1 && s.polygons > 0, "the floor bakes into several tiles");
    check(s.tilesRebuilt == s.tiles, "a first bake builds every tile");

    // Straight across the wall is 6 units; the path has to go round its end at x = 6.
    std::vector<glm::vec3> path;
    bool partial = true;
    check(mesh->findPath({-4, 0, -3}, {-4, 0, 3}, path, &partial), "a path is found");
    check(!partial && path.size() > 2, "the path is complete and turns corners");
    check(pathLength(path) > 20.0f, "the path goes round the wall");
    for (const glm::vec3& p : path) check(std::abs(p.y) < 0.5f, "the path stays on the floor");

    check(mesh->raycast({-4, 0, -3}, {-4, 0, 3}).has_value(), "a straight line hits the wall");
    check(!mesh->raycast({-4, 0, -3}, {4, 0, -3}).has_value(), "a line along the wall is clear");
    check(mesh->reachable({-4, 0, -3}, {-4, 0, 3}), "the far side is reachable");
    const auto snapped = mesh->nearestPoint({2, 3, 5});
    check(snapped && std::abs(snapped->y) < 0.5f, "nearestPoint drops onto the floor");
    check(!mesh->nearestPoint({100, 0, 100}).has_value(), "nothing near a point far off the mesh");
    check(!mesh->findPath({100, 0, 100}, {0, 0, 5}, path), "no path from off the mesh");

    // Close the gap: now the far side can't be reached, and paths come out partial.
    Transform& wall = *level.world.get<Transform>(level.wall);
    wall.position.x = 0;
    wall.scale.x = 21;
    const auto closed = bake(level.world, jobs, mesh.get());
    check(closed->stats().tilesRebuilt < closed->stats().tiles, "an edit rebuilds only the tiles it touched");
    check(!closed->reachable({-4, 0, -3}, {-4, 0, 3}), "a closed wall can't be walked round");
    check(closed->findPath({-4, 0, -3}, {-4, 0, 3}, path, &partial) && partial, "the path stops short");
}

void hashesAndFiles(JobSystem& jobs) {
    Level level;
    const NavMeshSettings settings = findNavMeshSettings(level.world);
    const uint64_t before = navInputHash(level.world, nullptr, settings);
    check(before == navInputHash(level.world, nullptr, settings), "the hash is stable");
    level.world.get<Transform>(level.wall)->position.z = 2;
    check(before != navInputHash(level.world, nullptr, settings), "moving a wall changes the hash");
    NavMeshSettings wider = settings;
    wider.agentRadius = 0.6f;
    check(
        navInputHash(level.world, nullptr, wider) != navInputHash(level.world, nullptr, settings),
        "the settings are part of the hash");

    // A dynamic body isn't walked on, so it doesn't change the bake.
    const uint64_t staticOnly = navInputHash(level.world, nullptr, settings);
    const Entity crate = box(level.world, {5, 3, 5}, {1, 1, 1});
    level.world.add<RigidBody>(crate, {});
    check(navInputHash(level.world, nullptr, settings) == staticOnly, "dynamic bodies are left out");
    // NavModifier Ignore leaves the wall out.
    level.world.add<NavModifier>(level.wall, {NavModifier::Mode::Ignore});
    const auto open = bake(level.world, jobs);
    std::vector<glm::vec3> path;
    check(
        open->findPath({-4, 0, -3}, {-4, 0, 3}, path) && pathLength(path) < 7.0f, "an ignored wall is walked through");

    const auto mesh = bake(level.world, jobs);
    const fs::path file = fs::temp_directory_path() / "shockwave-navigation-test.swnavmesh";
    std::string error;
    check(mesh->save(file, error), "save: " + error);
    const auto loaded = NavMesh::load(file, error);
    check(loaded != nullptr, "load: " + error);
    check(
        loaded->inputHash() == mesh->inputHash() && loaded->stats().polygons == mesh->stats().polygons,
        "a loaded mesh matches the saved one");
    check(loaded->findPath({-4, 0, -3}, {-4, 0, 3}, path), "a loaded mesh answers queries");
    fs::remove(file);
    check(navMeshPathFor("Scenes/level.swscene") == fs::path("Scenes/level.swnavmesh"), "mesh file sits by the scene");
    check(!NavMesh::load(file, error) && !error.empty(), "a missing file is an error");

    NavMeshSettings bad;
    bad.cellSize = 0;
    bad.agentRadius = -1;
    check(validateNavMeshSettings(bad).size() >= 2, "bad settings are reported");
    check(validateNavMeshSettings(NavMeshSettings{}).empty(), "the defaults are valid");
}

struct NoGame final : GameMode {};
struct TestInstance final : GameInstance {
    std::unique_ptr<GameMode> createGameMode(const World&) override { return std::make_unique<NoGame>(); }
};

void agentsWalk(JobSystem& jobs) {
    Level level;
    navcache::clear();
    TestInstance instance;
    const Entity agent = level.world.create();
    level.world.add<Transform>(agent, {glm::vec3(-4, 0, -3)});
    NavAgent a;
    a.destination = {-4, 0, 3};
    a.hasDestination = true;
    level.world.add<NavAgent>(agent, a);
    // A small crowd walking the other way through the same gap.
    for (int i = 0; i < 8; ++i) {
        const Entity e = level.world.create();
        level.world.add<Transform>(e, {glm::vec3(6.0f + float(i % 4), 0, 3.0f + float(i / 4))});
        NavAgent other;
        other.destination = {6.0f + float(i % 4), 0, -3.0f - float(i / 4)};
        other.hasDestination = true;
        level.world.add<NavAgent>(e, other);
    }
    // An agent standing far off the mesh is reported.
    const Entity lost = level.world.create();
    level.world.add<Transform>(lost, {glm::vec3(100, 0, 100)});
    level.world.add<NavAgent>(lost, {});

    PlaySession session(level.world, jobs, instance);
    session.start();
    NavigationWorld& navigation = *session.navigation();
    check(navigation.navMesh() && navigation.baked(), "Play bakes a mesh when none was saved");
    check(navigation.agentCount() == 10, "every agent joins the crowd");
    bool reportedLost = false;
    for (const NavDiagnostic& d : navigation.diagnostics()) reportedLost |= d.entity == lost;
    check(reportedLost, "an agent off the mesh is reported");

    std::vector<glm::vec3> path;
    bool sawPath = false;
    for (int i = 0; i < 60 * 20 && !level.world.get<NavAgent>(agent)->arrived; ++i) {
        session.update(1.0 / 60);
        sawPath |= navigation.agentPath(agent, path);
    }
    const glm::vec3 end = level.world.get<Transform>(agent)->position;
    check(level.world.get<NavAgent>(agent)->arrived, "the agent arrives");
    check(glm::length(end - glm::vec3(-4, 0, 3)) < 0.6f, "the agent ends at its destination");
    check(sawPath, "the agent's path can be read for drawing");
    check(navigation.averageStepMs() >= 0.0 && navigation.averageStepMs() < 5.0, "the crowd step is cheap");

    // Following a moving target, then stopping.
    const Entity target = level.world.create();
    level.world.add<Transform>(target, {glm::vec3(-6, 0, 6)});
    check(navigation.setTarget(agent, target), "setTarget");
    for (int i = 0; i < 60 * 5; ++i) session.update(1.0 / 60);
    check(glm::length(level.world.get<Transform>(agent)->position - glm::vec3(-6, 0, 6)) < 1.0f, "it follows a target");
    check(navigation.stop(agent) && !level.world.get<NavAgent>(agent)->hasDestination, "stop clears the goal");
    check(!navigation.setDestination(target, {0, 0, 0}), "setDestination needs a NavAgent");
    session.stop();

    // Play again with the same scene: the mesh comes from the cache, not a new bake.
    PlaySession again(level.world, jobs, instance);
    again.start();
    check(again.navigation()->navMesh() && !again.navigation()->baked(), "a second Play reuses the bake");
    again.stop();
}

void noNavigation(JobSystem& jobs) {
    World world;
    box(world, {0, 0, 0}, {10, 1, 10});
    TestInstance instance;
    PlaySession session(world, jobs, instance);
    session.start();
    check(session.navigation() && !session.navigation()->navMesh(), "scenes without navigation bake nothing");
    std::vector<glm::vec3> path;
    check(!session.navigation()->findPath({0, 0, 0}, {1, 0, 1}, path), "queries say no without a mesh");
    session.update(1.0 / 60);
    session.stop();
}

}   // namespace

int main() {
    try {
        registerNavigationComponents();
        JobSystem jobs(3);
        bakingAndPaths(jobs);
        hashesAndFiles(jobs);
        agentsWalk(jobs);
        noNavigation(jobs);
    } catch (const std::exception& e) {
        std::cerr << "navigation test failed: " << e.what() << '\n';
        return 1;
    }
    std::cout << "navigation test passed\n";
    return 0;
}
