// The engine's replication profiles: engine.pose.v1 and engine.rigidbody.v1.
#include <Multiplayer/Replication.h>

#include <Engine/Components.h>
#include <Engine/PhysicsComponents.h>

#include <cmath>

namespace shockwave::net {

namespace {

enum PoseField : size_t { kPosition, kRotation, kLinear, kAngular, kFlags };

const char* capturePose(const World& world, Entity e, const ReplicationContext&, ProfileValue& out) {
    return capturePoseFields(world, e, out);
}

void applyPose(World& world, Entity e, const ReplicationContext&, const ProfileValue& value) {
    applyPoseFields(world, e, value);
}

}   // namespace

// Network roots are unparented (--check), so their Transform is their world pose.
const char* capturePoseFields(const World& world, Entity e, ProfileValue& out) {
    const Transform* t = world.get<Transform>(e);
    if (!t) return "the entity has no Transform";
    setVec3(out.fields[kPosition], t->position.x, t->position.y, t->position.z);
    setQuat(out.fields[kRotation], t->rotation.x, t->rotation.y, t->rotation.z, t->rotation.w);
    glm::vec3 linear{0.0f}, angular{0.0f};
    uint8_t flags = 0;
    if (const RigidBody* body = world.get<RigidBody>(e)) {
        linear = body->linearVelocity;
        angular = body->angularVelocity;
    } else if (const CharacterController* character = world.get<CharacterController>(e)) {
        linear = character->velocity;
        if (character->grounded) flags |= PoseState::kGrounded;
    }
    setVec3(out.fields[kLinear], linear.x, linear.y, linear.z);
    setVec3(out.fields[kAngular], angular.x, angular.y, angular.z);
    out.fields[kFlags].u = flags;
    return nullptr;
}

void applyPoseFields(World& world, Entity e, const ProfileValue& value) {
    Transform* t = world.get<Transform>(e);
    if (!t) return;
    const auto& p = value.fields[kPosition].f;
    const auto& r = value.fields[kRotation].f;
    t->position = {p[0], p[1], p[2]};
    // Validated to length 0.99..1.01; stored exactly unit length.
    t->rotation = glm::normalize(glm::quat(r[3], r[0], r[1], r[2]));
    const auto& l = value.fields[kLinear].f;
    const auto& a = value.fields[kAngular].f;
    if (RigidBody* body = world.get<RigidBody>(e)) {
        body->linearVelocity = {l[0], l[1], l[2]};
        body->angularVelocity = {a[0], a[1], a[2]};
    } else if (CharacterController* character = world.get<CharacterController>(e)) {
        character->velocity = {l[0], l[1], l[2]};
        character->grounded = (value.fields[kFlags].u & PoseState::kGrounded) != 0;
    }
}

void registerEngineReplication(ReplicationRegistry& registry) {
    std::string why;
    ReplicationProfile pose;
    pose.id = kProfilePose;
    pose.name = "engine.pose.v1";
    pose.fields = poseFields();
    pose.requiredComponents = {"Transform"};
    pose.capture = capturePose;
    pose.apply = applyPose;
    if (!registry.addProfile(std::move(pose), why)) SW_LOG(LogNetwork, Error, "{}", why);

    ReplicationProfile body;
    body.id = kProfileRigidBody;
    body.name = "engine.rigidbody.v1";
    body.fields = poseFields();
    body.requiredComponents = {"Transform", "RigidBody", "Collider"};
    body.capture = capturePose;
    body.apply = applyPose;
    if (!registry.addProfile(std::move(body), why)) SW_LOG(LogNetwork, Error, "{}", why);
}

}   // namespace shockwave::net
