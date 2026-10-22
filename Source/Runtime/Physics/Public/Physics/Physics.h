#pragma once
#include <Engine/PhysicsComponents.h>
#include <Engine/Subsystem.h>
#include <Engine/World.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace shockwave {
class Assets;

struct ContactEvent {
    enum class Type { Begin, Stay, End };
    Type type;
    Entity a, b;
    glm::vec3 point{0.0f}, normal{0.0f};   // normal from a toward b
    bool sensor = false;
};
struct PhysicsDiagnostic {
    Entity entity;
    std::string message;
};
struct RayHit {
    Entity entity;
    glm::vec3 point{0.0f}, normal{0.0f};
    float distance = 0.0f;
};
// A character's motion between steps, for network prediction: what the host sends a
// client about its own character, and what the client predicted.
struct CharacterMotionState {
    glm::vec3 position{0.0f}, velocity{0.0f};
    bool grounded = false;
};
// How a session simulates online (docs/Networking.md, "Prediction"). The session's
// NetworkWorld picks it before the first step; changing it rebuilds every body.
//   Offline    the normal game.
//   Authority  the normal game, except that predicted characters move against static
//              collision only, exactly as their clients predict them.
//   Client     no Jolt step: static collision for queries, moving bodies and other
//              characters as kinematic proxies that follow their Transforms (the latest
//              state from the host), and the predicted character moved only by
//              predictCharacterStep. Contacts and sensor events are empty.
// Dynamic bodies become kinematic proxies on clients inside physics only; the
// components, and so the authored scene Stop restores, are unchanged.
enum class PhysicsNetworkMode : uint8_t { Offline, Authority, Client };
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
    void synchronize();   // creates/removes/rebuilds bodies; safe before first tick
    void step(double dt);
    bool addImpulse(Entity entity, const glm::vec3& impulse);
    bool addForce(Entity entity, const glm::vec3& force);
    bool setVelocity(Entity entity, const glm::vec3& linear, const glm::vec3& angular = glm::vec3(0));
    std::optional<RayHit> raycast(
        const glm::vec3& origin, const glm::vec3& direction, float maxDistance, Entity ignore = kNullEntity,
        bool includeSensors = false, int mask = 65535) const;
    // The same, ignoring every entity in `ignore` (lag-compensated shots leave out the
    // live players, whose past capsules they test instead).
    std::optional<RayHit> raycast(
        const glm::vec3& origin, const glm::vec3& direction, float maxDistance, std::span<const Entity> ignore,
        bool includeSensors = false, int mask = 65535) const;
    const std::vector<ContactEvent>& contacts() const;

    // ---- Network play ----
    void setNetworkMode(PhysicsNetworkMode mode);
    PhysicsNetworkMode networkMode() const;
    // The characters whose movement clients predict (replaces the previous set). Online
    // they ignore other characters and every non-static body when they move, in both
    // modes, so the host's step and a client's prediction agree.
    void setPredictedCharacters(std::span<const Entity> characters);
    bool predicted(Entity character) const;
    // A character's position, velocity and whether it stands on the ground. Empty for
    // anything that isn't a working character.
    std::optional<CharacterMotionState> characterState(Entity character) const;
    // Between steps only: puts the character at this state (its Transform, velocity and
    // contacts too) and drops a pending jump. False for a non-character or a non-finite
    // state.
    bool correctCharacter(Entity character, const CharacterMotionState& state);
    // One fixed step of one character, the same movement step() uses, against static
    // collision when it's predicted. Nothing else moves: no Jolt step, no other body, no
    // contacts or sensor events, no navigation. Writes the character's Transform and its
    // controller's velocity and grounded. False for a non-character or bad arguments.
    bool predictCharacterStep(Entity character, glm::vec3 desiredVelocity, bool jump, double fixedDt);
    // Client proxies: moves a body (or a character's proxy) to this pose now, as a
    // teleport. False for an entity without a working body or a non-finite pose.
    bool applyReplicaBodyPose(
        Entity entity, glm::vec3 position, glm::quat rotation, glm::vec3 linearVelocity, glm::vec3 angularVelocity);
    const std::vector<PhysicsDiagnostic>& diagnostics() const;
    uint32_t bodyCount() const;
    uint32_t activeBodyCount() const;
    uint32_t characterCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}   // namespace shockwave
