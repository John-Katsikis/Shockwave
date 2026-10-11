#pragma once

// Authored navigation data: plain components saved in scenes and edited in the
// Inspector. The Navigation module bakes a navigation mesh (the walkable surface, as
// polygons) from the scene's static geometry and moves NavAgents across it during Play.

#include <Engine/World.h>

#include <glm/glm.hpp>

namespace shockwave {

// Scene-wide bake settings. Give one entity one; without it the defaults below apply
// whenever the scene has NavAgents.
struct NavMeshSettings {
    // Which static geometry is walked on. Static colliders: Colliders without a RigidBody,
    // or with a Static, non-sensor one. Render meshes: MeshRenderer and ModelRenderer. Both
    // uses an entity's collider when it has one and its mesh otherwise.
    enum class Geometry : int { StaticColliders, RenderMeshes, Both };
    Geometry geometry = Geometry::Both;
    // The agent the mesh is baked for. Walls are kept agentRadius away, ceilings lower
    // than agentHeight are left out, ledges up to maxClimb are steps, slopes up to
    // maxSlope degrees are walkable.
    float agentRadius = 0.4f;
    float agentHeight = 1.8f;
    float maxClimb = 0.4f;
    float maxSlope = 45.0f;
    // Voxel size the geometry is sampled at: smaller is more exact and slower to bake.
    float cellSize = 0.2f;
    float cellHeight = 0.1f;
    int tileSize = 64;   // cells along a tile's side; tiles bake in parallel and rebuild alone
    // Only geometry inside these bounds is baked; with autoBounds, everything is.
    bool autoBounds = true;
    glm::vec3 boundsMin{-50.0f, -10.0f, -50.0f};
    glm::vec3 boundsMax{50.0f, 20.0f, 50.0f};
    int maxAgents = 128;   // crowd size during Play; more agents than this stand still
};

// Moves an entity across the navigation mesh during Play, steering round walls and
// other agents (DetourCrowd). Transform.position is the agent's feet. With a
// CharacterController on the same entity, the agent drives its desiredVelocity and
// physics does the moving; otherwise the agent moves the Transform itself.
struct NavAgent {
    enum class Avoidance : int { None, Low, Medium, High };
    float speed = 3.5f;   // units per second
    float acceleration = 8.0f;   // units per second squared
    float radius = 0.4f;
    float height = 1.8f;
    float stoppingDistance = 0.3f;   // arrives this close to the goal
    // The goal: the target entity when one is set, otherwise destination when
    // hasDestination is on, otherwise the agent stands still.
    Entity target;
    glm::vec3 destination{0.0f};
    bool hasDestination = false;
    Avoidance avoidance = Avoidance::Medium;   // how hard it looks for gaps between agents
    float separation = 1.0f;   // how strongly it keeps away from nearby agents, 0 off
    bool faceMovement = true;   // turns to face where it walks (without a CharacterController)
    bool enabled = true;
    // Runtime state, not saved:
    glm::vec3 velocity{0.0f};
    float remainingDistance = 0.0f;   // straight-line distance to the goal
    bool arrived = false;
    bool onNavMesh = false;
};

// Changes how one entity's geometry takes part in the bake.
struct NavModifier {
    // Walkable: baked even when the Geometry setting would skip it. Not walkable: blocks
    // agents but is never walked on. Ignore: left out of the bake.
    enum class Mode : int { Walkable, NotWalkable, Ignore };
    Mode mode = Mode::NotWalkable;
};

// Registers NavMeshSettings, NavAgent and NavModifier. Call once at startup on the main
// thread, like registerAudioComponents.
void registerNavigationComponents();

}   // namespace shockwave
