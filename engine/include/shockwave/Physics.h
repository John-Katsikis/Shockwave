#pragma once
#include <shockwave/World.h>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace shockwave {
class Assets;

struct Collider {
    enum class Shape : int { Box, Sphere, Capsule, ConvexModel, TriangleModel };
    Shape shape = Shape::Box;
    glm::vec3 halfExtents{0.5f};
    float radius = 0.5f;
    float halfHeight = 0.5f; // capsule cylinder; total height = 2 * (halfHeight + radius)
    glm::vec3 offset{0.0f};
    bool enabled = true;
    bool operator==(const Collider&) const = default;
};
struct RigidBody {
    enum class Motion : int { Static, Dynamic, Kinematic };
    Motion motion = Motion::Dynamic;
    float mass = 1.0f, friction = 0.5f, restitution = 0.0f;
    float linearDamping = 0.05f, angularDamping = 0.05f, gravityFactor = 1.0f;
    glm::vec3 linearVelocity{0.0f}, angularVelocity{0.0f}; // world units/s, radians/s
    int collisionLayer = 0, collisionMask = 65535; // 16 gameplay categories
    bool sensor = false, continuous = false, enabled = true;
};
struct PhysicsSettings {
    glm::vec3 gravity{0.0f,-9.81f,0.0f};
    int collisionSteps = 1; // Jolt substeps per fixed tick, 1..8
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
struct ContactEvent {
    enum class Type { Begin, Stay, End };
    Type type;
    Entity a, b;
    glm::vec3 point{0.0f}, normal{0.0f}; // normal from a toward b
    bool sensor = false;
};
struct PhysicsDiagnostic { Entity entity; std::string message; };
struct RayHit {
    Entity entity;
    glm::vec3 point{0.0f}, normal{0.0f};
    float distance = 0.0f;
};
struct PhysicsConfig {
    uint32_t maxBodies = 16384, maxBodyPairs = 65536, maxContactConstraints = 16384;
};
// Owns runtime physics state separately from the copyable ECS. One per Play session.
// synchronize/step belong to the main thread between gameplay schedule runs.
// Queries and impulses can be called by gameplay workers between steps; never
// mutate the world concurrently with synchronize/step. No Jolt headers in game code.
class PhysicsWorld {
public:
    PhysicsWorld(World& world, JobSystem& jobs, const Assets* assets = nullptr, PhysicsConfig config = {});
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    void synchronize(); // creates/removes/rebuilds bodies; safe before first tick
    void step(double dt);
    bool addImpulse(Entity entity, const glm::vec3& impulse);
    bool addForce(Entity entity, const glm::vec3& force);
    bool setVelocity(Entity entity, const glm::vec3& linear, const glm::vec3& angular = glm::vec3(0));
    std::optional<RayHit> raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                                 Entity ignore = kNullEntity, bool includeSensors = false, int mask = 65535) const;
    const std::vector<ContactEvent>& contacts() const;
    const std::vector<PhysicsDiagnostic>& diagnostics() const;
    uint32_t bodyCount() const;
    uint32_t activeBodyCount() const;
    uint32_t characterCount() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace shockwave
