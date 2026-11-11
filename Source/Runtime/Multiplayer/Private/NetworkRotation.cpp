// Map rotation (Multiplayer/NetworkWorld.h, docs/Networking.md "Map rotation"): the host
// announces the next map, clients prepare it while the current one plays on, and the host
// commits to the clients that are ready. The session then ends with MapRotation and every
// peer reconnects to the host's next session, whose nonce the commit carried; a rejoining
// player gets its slot back. Nothing of this session (objects, baselines, histories,
// events, IDs) carries over: the next one starts from nothing.
#include <Multiplayer/NetworkWorld.h>

#include "NetworkWorldImpl.h"

#include <Core/Log.h>

#include <algorithm>

namespace shockwave::net {

const char* rotationPhaseName(RotationPhase phase) {
    switch (phase) {
    case RotationPhase::Idle: return "idle";
    case RotationPhase::Preparing: return "preparing";
    case RotationPhase::Committed: return "committed";
    case RotationPhase::Aborted: return "aborted";
    }
    return "?";
}

namespace {

const char* reasonText(ReasonCode reason) {
    const char* name = reasonName(reason);
    return name ? name : "unknown";
}

// How long a client keeps trying to reach the next session, and the host keeps its slots,
// beyond the load deadline: the host may still be starting the map when a client arrives.
constexpr double kRejoinMinimumSeconds = 10;

}   // namespace

// ---- Host ----

uint16_t NetworkWorld::Impl::reservedFor(PeerId peer) const {
    for (const auto& [who, slot] : reservations)
        if (who == peer) return slot;
    return 0;
}

bool NetworkWorld::Impl::slotReserved(uint16_t slot) const {
    if (reservationUntil >= 0 && now >= reservationUntil) return false;
    return std::any_of(reservations.begin(), reservations.end(), [&](const auto& r) { return r.second == slot; });
}

void NetworkWorld::Impl::finishRotation(RotationPhase phase, NetworkEventKind kind, ReasonCode reason) {
    rotation.phase = phase;
    if (phase == RotationPhase::Aborted) {
        rotation.abortReason = reason;
        ++stats.rotationsAborted;
        SW_LOG(LogNetwork, Info, "Map rotation to \"{}\" called off ({})", rotation.level, reasonText(reason));
    }
    w.push({kind, w.m_state, {}, 0, reason});
}

void NetworkWorld::Impl::rotationPlayerLeft(uint16_t slot) {
    ++rosterRevision;
    if (rotation.phase == RotationPhase::Committed) {   // on its way to the next map
        for (RotationPlayer& p : rotation.players)
            if (p.slot == slot) p.left = true;
        return;
    }
    if (rotation.phase != RotationPhase::Preparing) return;
    auto& players = rotation.players;
    players.erase(
        std::remove_if(players.begin(), players.end(), [&](const RotationPlayer& p) { return p.slot == slot; }),
        players.end());
}

void NetworkWorld::Impl::checkRotation() {
    if (!host() || rotation.phase != RotationPhase::Preparing) return;
    const bool everyone =
        std::all_of(rotation.players.begin(), rotation.players.end(), [](const RotationPlayer& p) { return p.ready; });
    if (rotation.localReady && everyone) return commitRotation();
    if (!haveClock || now < rotation.deadline) return;
    if (!rotation.localReady) return w.abortRotation(ReasonCode::MapNotLoaded);
    if (!rotation.dropUnready) return w.abortRotation(ReasonCode::MapNotLoaded);
    // The deadline passed: whoever isn't ready stays behind, with a reason they can show.
    std::vector<uint16_t> late;
    for (const RotationPlayer& p : rotation.players)
        if (!p.ready) late.push_back(p.slot);
    for (const uint16_t slot : late) {
        ++stats.rotationDrops;
        if (Peer* p = peerForSlot(slot)) reject(*p, ReasonCode::MapNotLoaded, "the next map didn't load in time");
    }
    commitRotation();
}

void NetworkWorld::Impl::commitRotation() {
    if (!secureRandomBytes(rotation.nextSession)) return w.abortRotation(ReasonCode::ServiceUnavailable);
    MapCommit commit;
    commit.transitionId = rotation.transitionId;
    commit.nextEpoch = rotation.nextEpoch;
    commit.startTick = 0;
    commit.sessionNonce = rotation.nextSession;
    for (const RotationPlayer& player : rotation.players)
        if (Peer* p = peerForSlot(player.slot); p && player.ready) send(p->connection, commit);
    ++stats.rotationsCommitted;
    committedAt = now;
    SW_LOG(
        LogNetwork, Info, "Map rotation to \"{}\" committed: {} players follow", rotation.level,
        rotation.players.size());
    finishRotation(RotationPhase::Committed, NetworkEventKind::RotationCommitted, ReasonCode::Normal);
}

bool NetworkWorld::Impl::hostRotationMessage(Peer& p, const DecodedMessage& decoded) {
    const MessageKind kind = kindOf(decoded.body);
    if (kind != MessageKind::MapReady && kind != MessageKind::MapReject) return false;
    const uint32_t id = kind == MessageKind::MapReady ? std::get<MapReady>(decoded.body).transitionId
                                                      : std::get<MapReject>(decoded.body).transitionId;
    auto player = std::find_if(
        rotation.players.begin(), rotation.players.end(), [&](const RotationPlayer& r) { return r.slot == p.slot; });
    // An answer to an older rotation, or after this one was decided: nothing to do.
    if (rotation.phase != RotationPhase::Preparing || id != rotation.transitionId || player == rotation.players.end()) {
        if (id > rotation.transitionId) {
            reject(p, ReasonCode::MalformedPacket, "an answer to a map rotation that wasn't announced");
            return true;
        }
        ++rotation.duplicates;
        return true;
    }
    if (kind == MessageKind::MapReady) {
        const auto& ready = std::get<MapReady>(decoded.body);
        if (ready.nextEpoch != rotation.nextEpoch) {
            reject(p, ReasonCode::MalformedPacket, "MapReady for another map epoch");
            return true;
        }
        if (ready.contentDigest != w.m_start.compatibility.contentDigest) {
            player->rejected = ReasonCode::ContentMismatch;
        } else {
            if (player->ready) ++rotation.duplicates;
            player->ready = true;
            checkRotation();
            return true;
        }
    } else {
        const ReasonCode why = std::get<MapReject>(decoded.body).reason;
        player->rejected = why == ReasonCode::Normal ? ReasonCode::MapNotLoaded : why;
    }
    // It can't follow: it stays behind, or nobody goes.
    const ReasonCode why = player->rejected;
    if (!rotation.dropUnready) {
        w.abortRotation(why);
        return true;
    }
    ++stats.rotationDrops;
    reject(p, why, "couldn't load the next map");
    checkRotation();
    return true;
}

// ---- Client ----

bool NetworkWorld::Impl::clientRotationMessage(const DecodedMessage& decoded) {
    switch (kindOf(decoded.body)) {
    case MessageKind::MapPrepare: {
        const auto& prepare = std::get<MapPrepare>(decoded.body);
        if (!activated) return true;   // the host announces only to Active clients
        if (rotation.transitionId == prepare.transitionId && rotation.phase != RotationPhase::Idle) {
            ++rotation.duplicates;
            return true;
        }
        if (prepare.nextEpoch != w.m_start.mapEpoch + 1) {
            failClient(ReasonCode::MalformedPacket, "a map rotation out of order");
            return true;
        }
        rotation = MapRotation{};
        rotation.transitionId = prepare.transitionId;
        rotation.nextEpoch = prepare.nextEpoch;
        rotation.level = std::string(prepare.levelId);
        rotation.sceneId = prepare.sceneId;
        rotation.deadline = now + prepare.loadDeadlineMs / 1000.0;
        ++stats.rotationsPrepared;
        rotation.phase = RotationPhase::Preparing;   // so it can be answered, even with a refusal
        const NetworkCompatibility& ours = w.m_start.compatibility;
        if (prepare.schemaDigest != ours.schemaDigest || prepare.contentDigest != ours.contentDigest) {
            w.rejectRotation(ReasonCode::ContentMismatch);
            return true;
        }
        SW_LOG(LogNetwork, Info, "The host is changing the map to \"{}\"", rotation.level);
        w.push({NetworkEventKind::RotationPrepared, w.m_state, {}, 0, ReasonCode::Normal});
        return true;
    }
    case MessageKind::MapCommit: {
        const auto& commit = std::get<MapCommit>(decoded.body);
        if (rotation.phase == RotationPhase::Committed && commit.transitionId == rotation.transitionId) {
            ++rotation.duplicates;
            return true;
        }
        if (rotation.phase != RotationPhase::Preparing || commit.transitionId != rotation.transitionId ||
            !rotation.localReady || commit.nextEpoch != rotation.nextEpoch) {
            failClient(ReasonCode::MalformedPacket, "MapCommit for a map we didn't say we were ready for");
            return true;
        }
        rotation.nextSession = commit.sessionNonce;
        ++stats.rotationsCommitted;
        finishRotation(RotationPhase::Committed, NetworkEventKind::RotationCommitted, ReasonCode::Normal);
        return true;
    }
    case MessageKind::MapAbort: {
        const auto& abort = std::get<MapAbort>(decoded.body);
        if (abort.transitionId != rotation.transitionId || rotation.phase != RotationPhase::Preparing) {
            ++rotation.duplicates;
            return true;
        }
        rotation.returnTo = abort.returnTo;
        finishRotation(RotationPhase::Aborted, NetworkEventKind::RotationAborted, abort.reason);
        if (abort.returnTo == MapAbort::kMenu) failClient(abort.reason, "the host called the map change off");
        return true;
    }
    default: return false;
    }
}

void NetworkWorld::Impl::retryRejoin() {
    if (host() || retryAt < 0 || now < retryAt || !w.m_transport) return;
    retryAt = -1;
    ++stats.rejoinAttempts;
    auto connection = w.m_transport->connect(w.m_start.address);
    if (!connection) return failClient(ReasonCode::ServiceUnavailable, "couldn't reach the host's next map");
    peers[0] = Peer{};
    peers[0].connection = connection.value();
    enter(peers[0], Peer::Phase::AwaitingHello);
}

// ---- NetworkWorld ----

NetResult<void> NetworkWorld::prepareRotation(const MapRotationRequest& request) {
    Impl& impl = *m_impl;
    if (m_role != NetworkRole::Authority || m_closed || m_state != NetworkState::Active || !m_transport)
        return NetError{NetErrorCode::InvalidState, "only an active host changes maps"};
    if (impl.rotation.phase == RotationPhase::Preparing || impl.rotation.phase == RotationPhase::Committed)
        return NetError{NetErrorCode::InvalidState, "a map rotation is already under way"};
    MapPrepare prepare;
    prepare.levelId = request.level;
    if (request.level.size() > kMaxLevelIdBytes || prepare.invalid())
        return NetError{NetErrorCode::InvalidArgument, "not a level ID (letters, digits, _, - and .)"};
    if (request.sceneId == 0) return NetError{NetErrorCode::InvalidArgument, "no scene ID"};
    const double seconds = std::clamp(request.readySeconds, 1.0, 600.0);
    MapRotation r;
    r.transitionId = impl.rotation.transitionId + 1;
    r.nextEpoch = m_start.mapEpoch + 1;
    r.level = request.level;
    r.sceneId = request.sceneId;
    r.deadline = impl.now + seconds;
    r.dropUnready = request.dropUnready;
    r.phase = RotationPhase::Preparing;
    // Anyone still joining would arrive in a map that's about to end: they join the next one.
    for (Peer& p : impl.peers) {
        if (p.phase == Peer::Phase::Free) continue;
        if (p.phase != Peer::Phase::Active) {
            ++impl.stats.rotationRefusals;
            impl.reject(p, ReasonCode::MapRotation, "the map is changing; join again in a moment");
            continue;
        }
        r.players.push_back({p.slot, p.peer, false, ReasonCode::Normal});
    }
    prepare.transitionId = r.transitionId;
    prepare.nextEpoch = r.nextEpoch;
    prepare.sceneId = r.sceneId;
    prepare.schemaDigest = m_start.compatibility.schemaDigest;
    prepare.contentDigest = m_start.compatibility.contentDigest;
    prepare.rosterRevision = impl.rosterRevision;
    prepare.loadDeadlineMs = uint32_t(seconds * 1000);
    impl.rotation = std::move(r);
    prepare.levelId = impl.rotation.level;   // the copy that lives on
    for (const RotationPlayer& player : impl.rotation.players)
        if (Peer* p = impl.peerForSlot(player.slot)) impl.send(p->connection, prepare);
    ++impl.stats.rotationsPrepared;
    SW_LOG(
        LogNetwork, Info, "Changing the map to \"{}\": {} players have {:.0f} s to load it", impl.rotation.level,
        impl.rotation.players.size(), seconds);
    push({NetworkEventKind::RotationPrepared, m_state, {}, 0, ReasonCode::Normal});
    return {};
}

void NetworkWorld::rotationReady() {
    Impl& impl = *m_impl;
    if (m_closed || impl.rotation.phase != RotationPhase::Preparing || impl.rotation.localReady) return;
    impl.rotation.localReady = true;
    if (impl.host()) return impl.checkRotation();
    MapReady ready;
    ready.transitionId = impl.rotation.transitionId;
    ready.nextEpoch = impl.rotation.nextEpoch;
    ready.contentDigest = m_start.compatibility.contentDigest;
    if (!impl.send(impl.peers[0].connection, ready))
        impl.failClient(ReasonCode::ServiceUnavailable, "couldn't tell the host the next map is ready");
}

void NetworkWorld::rejectRotation(ReasonCode reason) {
    Impl& impl = *m_impl;
    if (m_closed || impl.rotation.phase != RotationPhase::Preparing) return;
    if (reason == ReasonCode::Normal) reason = ReasonCode::MapNotLoaded;
    if (impl.host()) return abortRotation(reason);
    MapReject message;
    message.transitionId = impl.rotation.transitionId;
    message.reason = reason;
    impl.send(impl.peers[0].connection, message);
    impl.finishRotation(RotationPhase::Aborted, NetworkEventKind::RotationAborted, reason);
}

void NetworkWorld::abortRotation(ReasonCode reason, uint8_t returnTo) {
    Impl& impl = *m_impl;
    if (m_closed || !impl.host() || impl.rotation.phase != RotationPhase::Preparing) return;
    MapAbort abort;
    abort.transitionId = impl.rotation.transitionId;
    abort.reason = reason;
    abort.returnTo = returnTo > MapAbort::kMenu ? MapAbort::kStay : returnTo;
    impl.rotation.returnTo = abort.returnTo;
    for (const RotationPlayer& player : impl.rotation.players)
        if (Peer* p = impl.peerForSlot(player.slot)) impl.send(p->connection, abort);
    impl.finishRotation(RotationPhase::Aborted, NetworkEventKind::RotationAborted, reason);
}

const MapRotation& NetworkWorld::rotation() const { return m_impl->rotation; }

bool NetworkWorld::rotationHandedOver() const {
    const Impl& impl = *m_impl;
    if (impl.rotation.phase != RotationPhase::Committed) return false;
    if (!impl.host() || !m_transport) return true;
    if (impl.now >= impl.committedAt + kHandoverSeconds) return true;
    return std::all_of(impl.rotation.players.begin(), impl.rotation.players.end(), [&](const RotationPlayer& p) {
        return !p.ready || p.left;
    });
}

NetworkStart NetworkWorld::nextStart() const {
    const Impl& impl = *m_impl;
    NetworkStart next = m_start;
    if (impl.rotation.phase != RotationPhase::Committed) return next;
    next.compatibility.sceneId = impl.rotation.sceneId;
    next.mapEpoch = impl.rotation.nextEpoch;
    next.presetSession = true;
    next.session = impl.rotation.nextSession;
    const double window = std::max(kRejoinMinimumSeconds, impl.rotation.deadline - impl.now + kRejoinMinimumSeconds);
    next.reservedSlots.clear();
    next.reservationSeconds = 0;
    next.rejoinSeconds = 0;
    if (impl.host()) {
        next.kicked = impl.kicked;
        for (const RotationPlayer& p : impl.rotation.players)
            if (p.ready) next.reservedSlots.emplace_back(p.peer, p.slot);
        next.reservationSeconds = window;
    } else {
        next.rejoinSeconds = window;
    }
    return next;
}

}   // namespace shockwave::net
