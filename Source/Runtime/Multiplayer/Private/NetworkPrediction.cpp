// Prediction and presentation (Multiplayer/NetworkWorld.h, docs/Networking.md
// "Prediction"; plan sections 8.2, 8.3, 9.2 and 9.3): a client moves its own
// PredictedCharacter with each command it sends, keeps what it predicted, checks it
// against the host's state for the same tick and replays its commands after a miss. The
// renderer is told where to draw through PresentationTransform: the controlled character
// blended between ticks with the correction hidden by a short offset, every other moving
// object interpolated behind the host's clock.
#include <Multiplayer/NetworkWorld.h>

#include "NetworkWorldImpl.h"

#include <Core/Log.h>
#include <Engine/PhysicsComponents.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace shockwave::net {

namespace {

constexpr uint8_t kResyncPrediction = 2;

// Profiles that start with poseFields(): fields 0..4 are position, rotation, linear and
// angular velocity, flags.
bool posePrefix(const ReplicationProfile& profile) {
    static constexpr const char* kNames[] = {"position", "rotation", "linear_velocity", "angular_velocity", "flags"};
    if (profile.fields.size() < 5) return false;
    for (size_t i = 0; i < 5; ++i)
        if (profile.fields[i].name != kNames[i]) return false;
    return true;
}

PoseState poseOf(const ProfileValue& v) {
    PoseState p;
    const auto& pos = v.fields[0].f;
    const auto& rot = v.fields[1].f;
    const auto& lin = v.fields[2].f;
    const auto& ang = v.fields[3].f;
    p.position = {pos[0], pos[1], pos[2]};
    p.rotation = {rot[0], rot[1], rot[2], rot[3]};
    p.linearVelocity = {lin[0], lin[1], lin[2]};
    p.angularVelocity = {ang[0], ang[1], ang[2]};
    p.flags = uint8_t(v.fields[4].u);
    return p;
}

bool finite(glm::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

}   // namespace

// ---- Who is predicted -----------------------------------------------------------------------

Entity NetworkWorld::Impl::controlledEntityNow() const {
    const World* world = w.m_world;
    if (!world || !committed) return kNullEntity;
    auto predictedCharacter = [&](const NetObject& o) {
        const NetworkObject* n = world->alive(o.entity) ? world->get<NetworkObject>(o.entity) : nullptr;
        return n && n->mode == NetworkObject::Mode::PredictedCharacter && world->has<CharacterController>(o.entity) &&
               posePrefix(*o.profile);
    };
    if (controlled) {
        auto it = byId.find(controlled);
        if (it != byId.end() && objects[it->second].owner == w.m_localSlot && predictedCharacter(objects[it->second]))
            return objects[it->second].entity;
    }
    // Active names what this player owned when it joined; a pawn spawned later is the
    // first predicted character it owns.
    for (const NetObject& o : objects)
        if (o.owner == w.m_localSlot && predictedCharacter(o)) return o.entity;
    return kNullEntity;
}

bool NetworkWorld::Impl::predictsControlled() const {
    return !host() && w.m_role == NetworkRole::Client && physics() && w.m_predictionMove && activated &&
           controlledEntityNow().valid();
}

bool NetworkWorld::predicting() const { return m_impl->predictsControlled(); }

void NetworkWorld::setPhysics(PhysicsWorld* physics) {
    m_physics = physics;
    if (!physics) return;
    physics->setNetworkMode(
        m_role == NetworkRole::Client      ? PhysicsNetworkMode::Client
        : m_role == NetworkRole::Authority ? PhysicsNetworkMode::Authority
                                           : PhysicsNetworkMode::Offline);
}

void NetworkWorld::prepareStep() {
    Impl& impl = *m_impl;
    if (!m_physics || !online() || !m_world) return;
    impl.predictedScratch.clear();
    if (authority()) {
        for (const NetObject& o : impl.objects) {
            if (o.owner == 0 || !m_world->alive(o.entity)) continue;
            const NetworkObject* n = m_world->get<NetworkObject>(o.entity);
            if (n && n->mode == NetworkObject::Mode::PredictedCharacter && m_world->has<CharacterController>(o.entity))
                impl.predictedScratch.push_back(o.entity);
        }
    } else if (impl.predictsControlled()) {
        impl.predictedScratch.push_back(impl.controlledEntityNow());
    }
    m_physics->setPredictedCharacters(impl.predictedScratch);
}

// ---- History --------------------------------------------------------------------------------

void NetworkWorld::Impl::clearPrediction() {
    for (Predicted& p : history) p.used = false;
    havePredicted = false;
    newestPredicted = oldestPredicted = 0;
}

void NetworkWorld::Impl::resetPrediction(const char* why) {
    if (!havePredicted) return;
    clearPrediction();
    ++stats.predictionResets;
    warning = why;
    warningAt = now;
    SW_LOG(LogNetwork, Warning, "Prediction reset: {}", why);
}

void NetworkWorld::Impl::predict(const InputCommand& c, Entity e) {
    const NetObject* o = objectOf(e);
    if (!o || c.incarnation != o->incarnation) return;   // a command for another life
    if (havePredicted && (predictedIncarnation != o->incarnation || c.targetTick != newestPredicted + 1))
        clearPrediction();   // a new life, or the ticks stopped being consecutive
    World& world = *w.m_world;
    const PredictedMove move = w.m_predictionMove(world, e, c);
    if (!finite(move.desiredVelocity)) return;
    if (!physics()->predictCharacterStep(e, move.desiredVelocity, move.jump, 1.0 / double(settings().tickRate))) return;
    const auto state = physics()->characterState(e);
    if (!state) return;
    Predicted& p = history[c.targetTick % NetworkWorld::kPredictionHistory];
    p = {c.targetTick, o->incarnation, move, *state, true};
    if (!havePredicted) oldestPredicted = c.targetTick;
    newestPredicted = c.targetTick;
    if (newestPredicted - oldestPredicted >= NetworkWorld::kPredictionHistory)
        oldestPredicted = newestPredicted - (NetworkWorld::kPredictionHistory - 1);
    predictedIncarnation = o->incarnation;
    havePredicted = true;
    ++stats.predictedTicks;
}

// ---- Reconciliation -------------------------------------------------------------------------

glm::vec3 NetworkWorld::Impl::visualOffset(double time) const {
    const double age = time - offsetAt;
    if (age >= NetworkWorld::kOffsetMaxSeconds || age < 0) return glm::vec3(0.0f);
    // Halves every 50 ms, and fades out over the last 50 ms so it ends without a jump.
    double k = std::exp2(-age / NetworkWorld::kOffsetHalfLife);
    const double fadeFrom = NetworkWorld::kOffsetMaxSeconds - NetworkWorld::kOffsetHalfLife;
    if (age > fadeFrom) k *= 1.0 - (age - fadeFrom) / NetworkWorld::kOffsetHalfLife;
    return offset * float(k);
}

void NetworkWorld::Impl::reconcile(NetObject& o, uint32_t tick, const CharacterMotionState& hostState) {
    World& world = *w.m_world;
    PhysicsWorld& physicsWorld = *physics();
    const Entity e = o.entity;
    ++stats.predictionChecks;
    const auto snap = [&] {
        physicsWorld.correctCharacter(e, hostState);
        if (auto* previous = world.get<PreviousTransform>(e)) previous->value = *world.get<Transform>(e);
        offset = glm::vec3(0.0f);
        offsetAt = -1e9;
    };
    if (!havePredicted || predictedIncarnation != o.incarnation) {
        // Nothing predicted yet (or only for an old life): the host's state is where to
        // start from.
        clearPrediction();
        snap();
        return;
    }
    if (newer(tick, newestPredicted)) {
        // The host is past every command predicted (none were sent for a while): take its
        // state and start predicting again from there.
        clearPrediction();
        snap();
        return;
    }
    if (newer(oldestPredicted, tick)) return;   // from before these predictions: already superseded
    const Predicted& h = history[tick % NetworkWorld::kPredictionHistory];
    if (!h.used || h.tick != tick || h.incarnation != o.incarnation) {
        resetPrediction("no prediction for the host's tick");
        snap();
        return;
    }
    const float error = glm::length(hostState.position - h.state.position);
    const bool match = error <= NetworkWorld::kPositionTolerance &&
                       glm::length(hostState.velocity - h.state.velocity) <= NetworkWorld::kVelocityTolerance &&
                       hostState.grounded == h.state.grounded;
    oldestPredicted = tick == newestPredicted ? tick : tick + 1;   // older ones are settled
    if (match) {
        ++stats.predictionMatches;
        return;
    }
    if (newestPredicted - tick > NetworkWorld::scaledTicks(NetworkWorld::kMaxReplayTicks, settings().tickRate)) {
        resetPrediction("the host's state is too old to replay from");
        snap();
        requestResync(kResyncPrediction);
        return;
    }
    const glm::vec3 before = world.get<Transform>(e)->position;
    physicsWorld.correctCharacter(e, hostState);
    const double dt = 1.0 / double(settings().tickRate);
    for (uint32_t t = tick + 1; !newer(t, newestPredicted); ++t) {
        Predicted& p = history[t % NetworkWorld::kPredictionHistory];
        if (!p.used || p.tick != t) break;
        // Movement only: the game's code ran when the command was first predicted, and
        // physics makes no contacts or sensor events here.
        physicsWorld.predictCharacterStep(e, p.move.desiredVelocity, p.move.jump, dt);
        if (const auto state = physicsWorld.characterState(e)) p.state = *state;
        ++stats.replayedTicks;
    }
    const glm::vec3 delta = world.get<Transform>(e)->position - before;
    // The blend from the previous tick moves with the correction, so only the offset
    // shows it.
    if (auto* previous = world.get<PreviousTransform>(e)) previous->value.position += delta;
    const glm::vec3 shown = visualOffset(now) - delta;
    if (glm::length(shown) > NetworkWorld::kSnapDistance) {
        offset = glm::vec3(0.0f);
        offsetAt = -1e9;
    } else {
        offset = shown;
        offsetAt = now;
    }
    ++stats.corrections;
    stats.lastCorrection = error;
    stats.maxCorrection = std::max(stats.maxCorrection, error);
}

void NetworkWorld::Impl::applyPredicted(NetObject& o, uint32_t tick) {
    World& world = *w.m_world;
    const Entity e = o.entity;
    const Transform kept = *world.get<Transform>(e);
    CharacterController* character = world.get<CharacterController>(e);
    const glm::vec3 velocity = character->velocity;
    const bool grounded = character->grounded;
    applyState(o);
    *world.get<Transform>(e) = kept;
    character = world.get<CharacterController>(e);
    character->velocity = velocity;
    character->grounded = grounded;
    ProfileValue value;
    if (!decodeProfile(*o.profile, std::span(o.state.data(), o.stateBytes), value)) return;
    const PoseState pose = poseOf(value);
    CharacterMotionState hostState;
    hostState.position = {pose.position.x, pose.position.y, pose.position.z};
    hostState.velocity = {pose.linearVelocity.x, pose.linearVelocity.y, pose.linearVelocity.z};
    hostState.grounded = (pose.flags & PoseState::kGrounded) != 0;
    reconcile(o, tick, hostState);
}

void NetworkWorld::Impl::newLife(NetObject& o) {
    World* world = w.m_world;
    if (host() || !world || !world->alive(o.entity)) return;
    if (auto* previous = world->get<PreviousTransform>(o.entity)) previous->value = *world->get<Transform>(o.entity);
    if (interpolation && o.slot != NetObject::kNoSlot) interpolation->clear(o.slot);
    if (activated && o.entity == controlledEntityNow()) {
        clearPrediction();
        offset = glm::vec3(0.0f);
        offsetAt = -1e9;
        const CharacterController* character = world->get<CharacterController>(o.entity);
        if (physics() && character)
            physics()->correctCharacter(
                o.entity, {world->get<Transform>(o.entity)->position, character->velocity, character->grounded});
    }
}

glm::vec3 NetworkWorld::correctionOffset() const { return m_impl->visualOffset(m_impl->now); }

const std::string& NetworkWorld::predictionWarning() const { return m_impl->warning; }

// ---- Interpolation --------------------------------------------------------------------------

void NetworkWorld::Impl::startInterpolation() {
    interpolation = std::make_unique<InterpolationEngine>(
        uint16_t(NetworkWorld::kMaxInterpolatedObjects), interpolationSettings(settings()));
    freeSlots.clear();
    for (uint32_t i = NetworkWorld::kMaxInterpolatedObjects; i-- > 0;) freeSlots.push_back(uint16_t(i));
}

void NetworkWorld::Impl::releaseSlot(NetObject& o) {
    if (o.slot == NetObject::kNoSlot) return;
    if (interpolation) interpolation->clear(o.slot);
    freeSlots.push_back(o.slot);
    o.slot = NetObject::kNoSlot;
}

void NetworkWorld::Impl::pushInterpolation(NetObject& o, uint32_t tick) {
    World* world = w.m_world;
    if (!interpolation || !world || !world->alive(o.entity) || !posePrefix(*o.profile)) return;
    const NetworkObject* n = world->get<NetworkObject>(o.entity);
    if (!n || n->mode == NetworkObject::Mode::Static) return;
    if (o.slot == NetObject::kNoSlot) {
        if (freeSlots.empty()) return;   // shown at its latest state instead
        o.slot = freeSlots.back();
        freeSlots.pop_back();
    }
    ProfileValue value;
    if (!decodeProfile(*o.profile, std::span(o.state.data(), o.stateBytes), value)) return;
    const PoseState pose = poseOf(value);
    if (pose.invalid()) return;
    // Incarnations start at 0, and 0 marks an empty slot here: 1..65535 instead.
    interpolation->push(o.slot, tick, quantizeState(pose, o.incarnation % 0xFFFFu + 1));
}

void NetworkWorld::present(float alpha) {
    Impl& impl = *m_impl;
    if (impl.now - impl.warningAt > kWarningSeconds) impl.warning.clear();
    impl.stats.interpolatedObjects = 0;
    if (m_role != NetworkRole::Client || !m_world || !impl.committed) return;
    World& world = *m_world;
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    const Entity mine = impl.predictsControlled() ? impl.controlledEntityNow() : kNullEntity;
    if (mine.valid()) {
        const Transform& t = *world.get<Transform>(mine);
        const PreviousTransform* previous = world.get<PreviousTransform>(mine);
        Transform shown = previous ? Transform::blend(previous->value, t, alpha) : t;
        shown.position += impl.visualOffset(impl.now);
        world.add<PresentationTransform>(mine, {shown});
    }
    const bool clock = impl.interpolation && impl.interpolation->hasClock();
    const NetTime at = clock ? impl.interpolation->renderTime(impl.now) : NetTime{};
    for (NetObject& o : impl.objects) {
        if (o.entity == mine || !world.alive(o.entity)) continue;
        InterpolatedPose pose;
        if (!clock || o.slot == NetObject::kNoSlot ||
            impl.interpolation->sample(o.slot, at, pose) == InterpolationResult::Empty) {
            if (world.has<PresentationTransform>(o.entity)) world.remove<PresentationTransform>(o.entity);
            continue;
        }
        Transform shown = *world.get<Transform>(o.entity);
        shown.position = pose.position;
        shown.rotation = pose.rotation;
        world.add<PresentationTransform>(o.entity, {shown});
        ++impl.stats.interpolatedObjects;
    }
}

}   // namespace shockwave::net
