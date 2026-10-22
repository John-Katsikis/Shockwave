// Combat history and game events (Multiplayer/NetworkWorld.h, docs/Networking.md "Combat
// and events"): the host's record of every player's capsule after each tick, shots tested
// against it at the time the shooter saw (plan 10.1), the host's pings that bound that
// time, and registered game events, sent reliably once each to Active clients.
#include <Multiplayer/NetworkWorld.h>

#include "NetworkWorldImpl.h"

#include <Core/Log.h>
#include <Engine/PhysicsComponents.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace shockwave::net {

namespace {

constexpr uint32_t kMaxHostPingsAnswered = 10;   // a second; a host pings once

bool finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

}   // namespace

const char* shotRewindName(ShotRewind rewind) {
    switch (rewind) {
    case ShotRewind::Present: return "present";
    case ShotRewind::Exact: return "exact";
    case ShotRewind::Clamped: return "clamped";
    case ShotRewind::Refused: return "refused";
    }
    return "?";
}

void NetworkWorld::Impl::clearCombat() {
    rewind.reset();
    rewindStates.clear();
    rewindEntities.clear();
    gameEventSeq = 0;
    haveGameEventSeq = false;
}

// ---- History --------------------------------------------------------------------------------

// After the tick's gameplay: each slot's character as it now is, from the game's
// description of it. The first owned object the game accepts is the slot's.
void NetworkWorld::Impl::recordCombat() {
    if (!host() || !w.m_combatTargets || !w.m_world) return;
    World& world = *w.m_world;
    if (!rewind) {
        rewind = std::make_unique<RewindHistoryBuffer>(settings());
        rewindStates.resize(rewind->playerSlots());
        rewindEntities.resize(rewind->playerSlots());
        shotIgnore.reserve(size_t(rewind->playerSlots()) + 1);
    }
    std::fill(rewindStates.begin(), rewindStates.end(), RewindPlayerState{});
    std::fill(rewindEntities.begin(), rewindEntities.end(), kNullEntity);
    for (const NetObject& o : objects) {
        if (o.owner == 0 || o.owner > rewindStates.size() || !world.alive(o.entity)) continue;
        const size_t slot = o.owner - 1u;
        if (rewindEntities[slot].valid()) continue;
        const auto* controller = world.get<CharacterController>(o.entity);
        const auto* transform = world.get<Transform>(o.entity);
        CombatTarget target;
        if (!controller || !transform || !w.m_combatTargets(world, o.entity, target)) continue;
        const float scale = std::abs(transform->scale.x);
        RewindPlayerState& state = rewindStates[slot];
        state.feet = transform->position;
        state.radius = controller->radius * scale;
        state.height = 2 * (controller->halfHeight + controller->radius) * scale;
        state.eye = state.feet + glm::vec3(0, target.eyeHeight, 0);
        state.incarnation = o.incarnation + 1;   // 0 means an empty slot in the history
        state.team = target.team;
        state.flags = uint8_t(
            (target.alive && controller->enabled ? RewindPlayerState::kAlive : 0) |
            (target.protectedFromHits ? RewindPlayerState::kProtected : 0));
        rewindEntities[slot] = o.entity;
    }
    rewind->record(tick, rewindStates);
}

bool NetworkWorld::resolveShot(const ShotQuery& shot, ShotResult& out) {
    out = ShotResult{};
    if (!authority() || !m_world) return false;
    if (!finite(shot.origin) || !finite(shot.direction) || !std::isfinite(shot.range) || shot.range <= 0) return false;
    const float length = glm::length(shot.direction);
    if (length < 1e-6f) return false;
    Impl& impl = *m_impl;
    World& world = *m_world;
    const glm::vec3 direction = shot.direction / length;
    ++impl.stats.shotsResolved;
    out.end = shot.origin + direction * shot.range;
    out.distance = shot.range;
    auto worldHit = [&](std::optional<RayHit> hit) {
        if (!hit) return;
        out.hit = hit->entity;
        out.end = hit->point;
        out.distance = hit->distance;
    };

    const NetworkIdentity* id = world.alive(shot.shooter) ? identity(shot.shooter) : nullptr;
    const uint16_t shooterSlot = id ? id->ownerSlot : 0;
    RewindHistoryBuffer* history = impl.host() ? impl.rewind.get() : nullptr;
    if (!history || !history->hasLatest()) {
        // Offline, or nothing recorded yet: the present, as the physics world has it.
        if (m_physics) worldHit(m_physics->raycast(shot.origin, direction, shot.range, shot.shooter));
        if (out.hit.valid()) ++impl.stats.worldHits;
        return true;
    }

    // When the shooter's view showed the targets.
    const uint32_t rate = m_start.settings.tickRate;
    uint32_t viewTick = history->latestTick();
    float fraction = 0;
    out.rewind = ShotRewind::Present;
    if (shot.command && shooterSlot != m_localSlot) {
        out.rewind = ShotRewind::Exact;
        // Its time relative to when the command reached the host (A): a client shows
        // remote players a round trip plus the interpolation delay behind A, never later
        // than A minus the delay.
        uint32_t arrival = impl.tick;
        if (Peer* p = impl.peerForSlot(shooterSlot)) {
            const size_t index = shot.command->targetTick % kInputHistory;
            if (p->inputs[index].targetTick == shot.command->targetTick) arrival = p->inputArrival[index];
        }
        const double delay = interpolationSettings(m_start.settings).delayTicks;
        const double tolerance = double(scaledTicks(kViewToleranceTicks, rate)) +
                                 double(rate) / double(std::max<uint32_t>(1, m_start.settings.snapshotRate));
        double view = double(int32_t(shot.command->viewTick - arrival)) + double(shot.command->viewFraction) / 65536.0;
        const double newest = -delay + tolerance;
        const double rtt = impl.peerForSlot(shooterSlot) ? impl.peerRoundTrip(*impl.peerForSlot(shooterSlot)) : 0;
        const double oldest = rtt > 0 ? -rtt * rate - delay - tolerance : -std::numeric_limits<double>::infinity();
        if (shot.command->viewTick == 0 || view < oldest) {
            out.rewind = ShotRewind::Refused;   // a time the client can't have seen
        } else if (view > newest) {
            view = newest;
            out.rewind = ShotRewind::Clamped;
        }
        const double at = double(arrival) + view;
        viewTick = uint32_t(int64_t(std::floor(at)));
        fraction = float(at - std::floor(at));
    }
    out.viewTick = viewTick;
    out.viewFraction = fraction;

    // Players where they were then, unless the time was refused.
    RewindHit player;
    bool playerHit = false;
    if (out.rewind != ShotRewind::Refused) {
        RewindQuery query;
        const uint16_t self = shooterSlot ? uint16_t(shooterSlot - 1) : 0xFFFF;
        playerHit =
            history->raycastAtTick(shot.origin, direction, shot.range, viewTick, fraction, self, player, &query);
        if (query.status == RewindStatus::Rejected) {
            out.rewind = ShotRewind::Refused;
            playerHit = false;
        } else if (query.status == RewindStatus::Clamped && out.rewind == ShotRewind::Exact) {
            out.rewind = ShotRewind::Clamped;
        }
    }
    switch (out.rewind) {
    case ShotRewind::Exact: ++impl.stats.shotsRewound; break;
    case ShotRewind::Clamped: ++impl.stats.shotsClamped; break;
    case ShotRewind::Refused: ++impl.stats.shotsRefused; break;
    case ShotRewind::Present: break;
    }

    // Everything else as it is now, without the live characters (their past capsules
    // stand in for them) or the shooter. A player counts only in front of it.
    impl.shotIgnore.clear();
    impl.shotIgnore.push_back(shot.shooter);
    for (const Entity e : impl.rewindEntities)
        if (e.valid()) impl.shotIgnore.push_back(e);
    if (m_physics) worldHit(m_physics->raycast(shot.origin, direction, shot.range, impl.shotIgnore));
    if (playerHit && player.distance < out.distance && player.slot < impl.rewindEntities.size()) {
        const Entity target = impl.rewindEntities[player.slot];
        if (world.alive(target)) {
            out.hit = target;
            out.player = true;
            out.end = player.point;
            out.distance = player.distance;
            out.hitIncarnation = player.incarnation - 1;
        }
    }
    if (out.player)
        ++impl.stats.playerHits;
    else if (out.hit.valid())
        ++impl.stats.worldHits;
    return true;
}

// ---- Round trips ----------------------------------------------------------------------------

void NetworkWorld::Impl::sendHostPings() {
    for (Peer& p : peers) {
        if (p.phase != Peer::Phase::Active || now < p.nextHostPing) continue;
        Ping ping;
        ping.pingId = ++p.hostPingId;
        ping.originUs = uint64_t(std::max(0.0, now) * 1e6);
        if (send(p.connection, ping)) ++stats.pingsSent;
        // Four quick probes at first, then one a second.
        p.nextHostPing = now + (++p.hostPings < 4 ? 0.1 : NetworkWorld::kHostPingSeconds);
    }
}

void NetworkWorld::Impl::hostPong(Peer& p, const Pong& pong) {
    if (pong.pingId == 0 || pong.pingId > p.hostPingId || p.hostPingId - pong.pingId > 16) return;
    const double nowUs = std::max(0.0, now) * 1e6;
    if (double(pong.echoedOriginUs) > nowUs) return;
    const double dwell = double(pong.responderSendUs - pong.responderReceiveUs) / 1e6;
    const double rtt = (nowUs - double(pong.echoedOriginUs)) / 1e6 - dwell;
    if (!(rtt >= 0) || rtt > 5) return;
    ++stats.pongsReceived;
    p.rtts[p.rttCount % p.rtts.size()] = rtt;
    ++p.rttCount;
    double worst = 0;
    for (const Peer& q : peers)
        if (q.phase == Peer::Phase::Active) worst = std::max(worst, peerRoundTrip(q));
    stats.rttMs = worst * 1000;
}

void NetworkWorld::Impl::clientPing(Peer& h, const Ping& ping) {
    if (h.pingWindow < 0 || now - h.pingWindow >= 1) {
        h.pingWindow = now;
        h.pings = 0;
    }
    if (++h.pings > kMaxHostPingsAnswered) return;
    Pong pong;
    pong.pingId = ping.pingId;
    pong.echoedOriginUs = ping.originUs;
    pong.responderReceiveUs = pong.responderSendUs = uint64_t(std::max(0.0, now) * 1e6);
    pong.responderTick = tick;
    if (send(h.connection, pong)) ++stats.pingsAnswered;
}

double NetworkWorld::Impl::peerRoundTrip(const Peer& p) const {
    const uint32_t n = std::min<uint32_t>(p.rttCount, uint32_t(p.rtts.size()));
    double worst = 0;
    for (uint32_t i = 0; i < n; ++i) worst = std::max(worst, p.rtts[i]);
    return worst;
}

double NetworkWorld::roundTrip(uint16_t slot) const {
    if (m_role == NetworkRole::Client) return m_impl->bestRtt();
    const Peer* p = m_impl->peerForSlot(slot);
    return p ? m_impl->peerRoundTrip(*p) : 0;
}

// ---- Game events ----------------------------------------------------------------------------

ReceivedGameEvent& NetworkWorld::Impl::storeGameEvent() {
    ReceivedGameEvent& e = gameEvents[gameEventCount % NetworkWorld::kGameEventHistory];
    ++gameEventCount;
    e = ReceivedGameEvent{};
    return e;
}

NetResult<void> NetworkWorld::sendEvent(uint16_t eventId, Entity subject, const ProfileValue& payload) {
    if (!authority() || m_closed) return NetError{NetErrorCode::InvalidState, "only the host sends game events"};
    Impl& impl = *m_impl;
    const ReplicationRegistry* registry = m_start.registry;
    const EventDesc* desc = registry ? registry->event(eventId) : nullptr;
    if (registry && !desc) return NetError{NetErrorCode::InvalidArgument, "that game event isn't registered"};
    uint32_t written = 0;
    if (desc && !encodeFields(desc->payload, payload, impl.eventBytes, written))
        return NetError{NetErrorCode::InvalidArgument, "the event's payload is out of its ranges"};
    const uint64_t subjectId = m_world && m_world->alive(subject) ? networkId(subject) : 0;
    ++impl.gameEventSeq;
    ReceivedGameEvent& e = impl.storeGameEvent();
    e.event = desc;
    e.eventId = eventId;
    e.sequence = impl.gameEventSeq;
    e.tick = impl.tick;
    e.subject = subjectId;
    e.subjectEntity = subject;
    e.payload = payload;
    ++impl.stats.gameEventsSent;
    if (!impl.host() || !m_transport) return {};
    GameEvent message;
    message.eventSeq = impl.gameEventSeq;
    message.eventTick = impl.tick;
    message.subject = subjectId;
    message.eventId = eventId;
    message.payload = std::span<const uint8_t>(impl.eventBytes.data(), written);
    // Only Active clients: one joining later gets the durable state from its baseline and
    // snapshots, never old events. A client whose queue is full is dropped, as for spawns.
    std::array<bool, kMaxTransportConnections> full{};
    for (size_t i = 0; i < impl.peers.size(); ++i)
        if (impl.peers[i].phase == Peer::Phase::Active) full[i] = !impl.enqueue(impl.peers[i], message);
    for (size_t i = 0; i < impl.peers.size(); ++i)
        if (full[i] && impl.peers[i].admitted())
            impl.reject(impl.peers[i], ReasonCode::RateLimited, "fell too far behind the host");
    return {};
}

void NetworkWorld::Impl::gameEventMessage(const GameEvent& m) {
    if (!activated) return;   // nothing is sent before Active
    if (haveGameEventSeq && !newer(m.eventSeq, gameEventSeq)) {
        ++stats.duplicateEvents;   // already handled: never twice
        return;
    }
    const ReplicationRegistry* registry = w.m_start.registry;
    const EventDesc* desc = registry ? registry->event(m.eventId) : nullptr;
    if (!desc) return failClient(ReasonCode::MalformedPacket, "the host sent a game event this build doesn't know");
    ProfileValue value;
    if (!decodeFields(desc->payload, m.payload, value))
        return failClient(ReasonCode::MalformedPacket, "a game event's payload breaks its rules");
    gameEventSeq = m.eventSeq;
    haveGameEventSeq = true;
    ReceivedGameEvent& e = storeGameEvent();
    e.event = desc;
    e.eventId = m.eventId;
    e.sequence = m.eventSeq;
    e.tick = m.eventTick;
    e.subject = m.subject;
    e.subjectEntity = m.subject ? w.entity(m.subject) : kNullEntity;
    e.payload = value;
    ++stats.gameEventsReceived;
}

bool NetworkWorld::readGameEvent(uint64_t& cursor, ReceivedGameEvent& out) const {
    const Impl& impl = *m_impl;
    if (cursor >= impl.gameEventCount) return false;
    if (impl.gameEventCount - cursor > kGameEventHistory) {
        m_impl->stats.gameEventsMissed += impl.gameEventCount - kGameEventHistory - cursor;
        cursor = impl.gameEventCount - kGameEventHistory;
    }
    out = impl.gameEvents[cursor % kGameEventHistory];
    ++cursor;
    return true;
}

uint64_t NetworkWorld::gameEventCursor() const { return m_impl->gameEventCount; }

}   // namespace shockwave::net
