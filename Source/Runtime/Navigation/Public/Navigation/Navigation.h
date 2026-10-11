#pragma once

// NavigationWorld: the navigation of one Play session, as a subsystem of it
// (game.navigation()). When Play starts it finds the scene's navigation mesh: one baked
// or loaded earlier with the same inputs, or a fresh bake on the job system. Each fixed
// tick, before physics, it moves every NavAgent with DetourCrowd.
//
//     game.navigation().setDestination(guard, {10, 0, 4});
//     game.navigation().setTarget(guard, game.find("Player"));
//     if (auto p = game.navigation().nearestPoint(click)) ...
//     std::vector<glm::vec3> path;
//     if (game.navigation().findPath(a, b, path)) ...
//
// Threading: step and synchronize belong to the main thread, between schedule runs.
// findPath, nearestPoint, raycast and reachable are safe from any thread, including
// systems, at any time during Play. setDestination, setTarget and stop write the
// agent's NavAgent component, so call them on the main thread or from a system that
// writes NavAgent.

#include <Engine/EntityRef.h>
#include <Engine/Subsystem.h>
#include <Navigation/NavComponents.h>
#include <Navigation/NavMesh.h>
#include <Renderer/RenderTypes.h>

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace shockwave {

class Assets;
class JobSystem;
class Renderer;

struct NavDiagnostic {
    Entity entity;   // null for scene-wide problems
    std::string message;
};

class NavigationWorld final : public Subsystem {
public:
    // Finds or bakes the mesh when the scene uses navigation; otherwise does nothing.
    NavigationWorld(World& world, JobSystem& jobs, const Assets* assets = nullptr);
    ~NavigationWorld() override;
    NavigationWorld(const NavigationWorld&) = delete;
    NavigationWorld& operator=(const NavigationWorld&) = delete;

    // Main thread: adds and removes crowd agents to match the World's NavAgents. Play
    // calls it after beginPlay and every tick.
    void synchronize();
    // Main thread, once per fixed tick, before the physics step.
    void step(double dt);

    // ---- Moving agents ----
    // Walk to a point. False when the entity has no NavAgent.
    bool setDestination(Entity agent, const glm::vec3& destination);
    bool setDestination(const EntityRef& agent, const glm::vec3& destination) {
        return setDestination(agent.entity(), destination);
    }
    // Follow another entity; the path is renewed as it moves. kNullEntity stops following.
    bool setTarget(Entity agent, Entity target);
    bool setTarget(const EntityRef& agent, const EntityRef& target) {
        return setTarget(agent.entity(), target.entity());
    }
    // Stand still: clears the target and the destination.
    bool stop(Entity agent);
    bool stop(const EntityRef& agent) { return stop(agent.entity()); }

    // ---- Queries (any thread) ----
    bool findPath(
        const glm::vec3& start, const glm::vec3& end, std::vector<glm::vec3>& points, bool* partial = nullptr) const;
    std::optional<glm::vec3> nearestPoint(const glm::vec3& p, const glm::vec3& extents = {2.0f, 4.0f, 2.0f}) const;
    std::optional<glm::vec3> raycast(const glm::vec3& start, const glm::vec3& end) const;
    bool reachable(const glm::vec3& start, const glm::vec3& end) const;

    // ---- State ----
    // The session's mesh; null when the scene doesn't use navigation or the bake failed.
    const NavMesh* navMesh() const;
    std::shared_ptr<const NavMesh> sharedNavMesh() const;
    bool baked() const;   // true when the mesh was baked for this Play, not reused
    // Problems found when Play started and since: a failed bake, agents off the mesh,
    // targets they can't reach, agents wider than the mesh was baked for.
    const std::vector<NavDiagnostic>& diagnostics() const;
    uint32_t agentCount() const;
    // CPU time of the last step and its average over the last 120 ticks, in ms.
    double lastStepMs() const;
    double averageStepMs() const;
    // The path an agent is walking, from where it is to its goal, into points (cleared
    // first). Main thread, between ticks. False when it isn't walking anywhere.
    bool agentPath(Entity agent, std::vector<glm::vec3>& points) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Draws a navigation mesh and agents' paths into the scene: the editor's overlay, and
// player --view navmesh. Colors are linear RGB; the editor passes its theme's.
// Main thread, from Application::onRender after setCamera.
class NavMeshOverlay {
public:
    struct Style {
        glm::vec4 mesh{0.0f, 0.55f, 0.75f, 0.45f};
        glm::vec4 path{1.0f, 0.55f, 0.05f, 1.0f};
    };
    NavMeshOverlay() = default;
    ~NavMeshOverlay();
    NavMeshOverlay(const NavMeshOverlay&) = delete;
    NavMeshOverlay& operator=(const NavMeshOverlay&) = delete;

    // Uploads the mesh's triangles the first time it's drawn (and again for a new mesh).
    void drawMesh(Renderer& renderer, const std::shared_ptr<const NavMesh>& mesh, const Style& style);
    // Each agent's path as a line of thin boxes, ending in a marker at its goal.
    void drawPaths(
        Renderer& renderer, const Assets& assets, const NavigationWorld& navigation, const World& world,
        const Style& style);
    // Frees the GPU copy. Call before the renderer goes away.
    void release(Renderer& renderer);

private:
    std::shared_ptr<const NavMesh> m_uploaded;
    MeshHandle m_mesh;
    std::vector<glm::vec3> m_points;
};

}   // namespace shockwave
