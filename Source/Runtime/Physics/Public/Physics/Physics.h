#pragma once
#include <Engine/PhysicsComponents.h>
#include <Engine/Subsystem.h>
#include <Engine/World.h>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace shockwave {
class Assets;

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
// Owns runtime physics state separately from the copyable ECS. One per Play session,
// as a subsystem of it (PlaySession::subsystems()).
// synchronize/step belong to the main thread between gameplay schedule runs.
// Queries and impulses can be called by gameplay workers between steps; never
// mutate the world concurrently with synchronize/step. No Jolt headers in game code.
class PhysicsWorld final : public Subsystem {
public:
    PhysicsWorld(World& world, JobSystem& jobs, const Assets* assets = nullptr, PhysicsConfig config = {});
    ~PhysicsWorld() override;
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
