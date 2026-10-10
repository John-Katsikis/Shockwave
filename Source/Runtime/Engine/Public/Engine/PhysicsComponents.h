#pragma once

// Authored physics data. These are plain components saved in scenes and edited in the
// inspector; the Physics module turns them into Jolt bodies during Play.

#include <glm/glm.hpp>

namespace shockwave {

struct Collider {
    enum class Shape : int { Box, Sphere, Capsule, ConvexModel, TriangleModel };
    Shape shape = Shape::Box;
    glm::vec3 halfExtents{0.5f};
    float radius = 0.5f;
    float halfHeight = 0.5f;   // capsule cylinder; total height = 2 * (halfHeight + radius)
    glm::vec3 offset{0.0f};
    bool enabled = true;
    bool operator==(const Collider&) const = default;
};
struct RigidBody {
    enum class Motion : int { Static, Dynamic, Kinematic };
    Motion motion = Motion::Dynamic;
    float mass = 1.0f, friction = 0.5f, restitution = 0.0f;
    float linearDamping = 0.05f, angularDamping = 0.05f, gravityFactor = 1.0f;
    glm::vec3 linearVelocity{0.0f}, angularVelocity{0.0f};   // world units/s, radians/s
    int collisionLayer = 0, collisionMask = 65535;   // 16 gameplay categories
    bool sensor = false, continuous = false, enabled = true;
};
struct PhysicsSettings {
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    int collisionSteps = 1;   // Jolt substeps per fixed tick, 1..8
};
// Feet are at Transform.position; capsule is upright in world space. Give gameplay
// a desired world-space horizontal velocity. Jump is a one-shot runtime request.
// The controller supplies gravity, ground following, stairs and body interaction.
struct CharacterController {
    float radius = 0.35f, halfHeight = 0.5f, mass = 70.0f;
    float maxSlope = 45.0f, stepHeight = 0.3f, jumpSpeed = 5.0f, maxPushForce = 100.0f;
    int collisionLayer = 0, collisionMask = 65535;
    bool enabled = true;
    // Runtime state, not authored/serialized:
    glm::vec3 desiredVelocity{0.0f}, velocity{0.0f};
    bool jumpRequested = false, grounded = false;
};

}   // namespace shockwave
