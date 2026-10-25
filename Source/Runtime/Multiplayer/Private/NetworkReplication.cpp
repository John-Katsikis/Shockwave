// Replication (Multiplayer/NetworkWorld.h, docs/Networking.md "Replication"): network
// IDs, the baseline, lifecycle messages in control-serial order, full-state snapshots
// within each peer's budget, and the client side that stages, checks and applies them
// (handing the controlled character's state to prediction, NetworkPrediction.cpp).
#include <Multiplayer/NetworkWorld.h>

#include "NetworkWorldImpl.h"

#include <Core/Log.h>
#include <Engine/Hierarchy.h>
#include <Engine/PhysicsComponents.h>
#include <Engine/Prefab.h>

#include <algorithm>
#include <cstring>

namespace shockwave::net {

namespace {

constexpr uint8_t kResyncSnapshotExpired = 1;

// Each queued reliable message: u32 length, u32 control serial (0 if it has none), bytes.
constexpr size_t kQueueEntryHeader = 8;

uint32_t readU32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void writeU32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

const NetworkPrefab* findPrefab(const NetworkStart& start, uint16_t id) {
    for (const NetworkPrefab& p : start.prefabs)
        if (p.id == id) return &p;
    return nullptr;
}

Entity spawnCopy(const NetworkStart& start, World& world, const NetworkPrefab& prefab, const Transform& at) {
    if (start.spawnPrefab) return start.spawnPrefab(world, prefab, at);
    return spawnPrefab(world, prefab.path, &at);
}

// The control serial a queued message brings the peer to.
uint32_t serialOf(const MessageBody& body) {
    if (const auto* m = std::get_if<Spawn>(&body)) return m->controlSerial;
    if (const auto* m = std::get_if<Despawn>(&body)) return m->controlSerial;
    if (const auto* m = std::get_if<Ownership>(&body)) return m->controlSerial;
    if (const auto* m = std::get_if<BaselineEnd>(&body)) return m->finalControlSerial;
    return 0;
}

}   // namespace

// ---- Objects --------------------------------------------------------------------------------

NetObject* NetworkWorld::Impl::object(uint64_t id) {
    auto it = byId.find(id);
    return it == byId.end() ? nullptr : &objects[it->second];
}

NetObject* NetworkWorld::Impl::objectOf(Entity entity) {
    if (!w.m_world) return nullptr;
    const NetworkIdentity* identity = w.m_world->get<NetworkIdentity>(entity);
    if (!identity) return nullptr;
    NetObject* o = object(identity->id);
    return o && o->entity == entity ? o : nullptr;
}

// IDs only grow, so appending keeps objects sorted.
void NetworkWorld::Impl::insert(NetObject o) {
    byId[o.id] = uint32_t(objects.size());
    objects.push_back(std::move(o));
}

void NetworkWorld::Impl::erase(uint64_t id) {
    auto it = byId.find(id);
    if (it == byId.end()) return;
    const uint32_t index = it->second;
    releaseSlot(objects[index]);
    byId.erase(it);
    objects.erase(objects.begin() + index);
    for (uint32_t i = index; i < objects.size(); ++i) byId[objects[i].id] = i;
}

void NetworkWorld::Impl::writeIdentity(const NetObject& o) {
    if (w.m_world && w.m_world->alive(o.entity))
        w.m_world->add<NetworkIdentity>(o.entity, {o.id, o.owner, o.incarnation});
}

NetResult<void> NetworkWorld::Impl::beginReplication() {
    clearReplication();
    const uint32_t capacity = std::clamp<uint32_t>(settings().maxReplicatedEntities, 12, kMaxNetworkObjects);
    objects.reserve(capacity);
    byId.reserve(capacity);
    pick.reserve(capacity);
    partEnds.reserve(256);
    World* world = w.m_world;
    if (!world) return {};
    std::vector<std::pair<std::string, Entity>> found;
    world->each<const NetworkObject>([&](Entity e, const NetworkObject& n) { found.emplace_back(n.key, e); });
    if (found.empty()) return {};
    const ReplicationRegistry* registry = w.m_start.registry;
    if (!registry || !registry->frozen())
        return NetError{
            NetErrorCode::InvalidState, "the scene has network objects, but the session has no frozen registry"};
    if (found.size() > capacity)
        return NetError{
            NetErrorCode::InvalidArgument, "the scene has more network objects than max_replicated_entities"};
    // Numbered in key order, so the host's IDs don't depend on entity order.
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < found.size(); ++i) {
        const auto& [key, e] = found[i];
        auto bad = [&](const char* what) {
            beginError = "network object \"" + key + "\": " + what + " (see player --check)";
            return NetError{NetErrorCode::InvalidArgument, beginError.c_str()};
        };
        if (!validNetworkKey(key)) return bad("its key is missing or invalid");
        if (i > 0 && found[i - 1].first == key) return bad("two network objects have this key");
        if (world->has<PrefabPart>(e)) return bad("prefab parts can't be network objects in v1");
        if (parentOf(*world, e).valid()) return bad("network objects must be unparented");
        const NetworkObject& n = *world->get<NetworkObject>(e);
        const ReplicationProfile* profile = registry->profile(n.profile);
        if (!profile) return bad("its profile isn't registered");
        if (host()) {
            NetObject o;
            o.id = nextId++;
            o.key = key;
            o.profile = profile;
            o.entity = e;
            o.isStatic = n.mode == NetworkObject::Mode::Static;
            o.serial = ++serial;
            if (!captureState(o)) return bad("its state is outside its profile's ranges");
            writeIdentity(o);
            insert(std::move(o));
        } else {
            localKeys.emplace(key, e);
        }
    }
    return {};
}

void NetworkWorld::Impl::clearReplication() {
    clearCombat();
    std::vector<NetObject>().swap(objects);
    std::unordered_map<uint64_t, uint32_t>().swap(byId);
    std::unordered_map<std::string, Entity>().swap(localKeys);
    std::vector<uint32_t>().swap(pick);
    std::vector<uint32_t>().swap(partEnds);
    for (HeldSnapshot& h : held) h.used = false;
    baselineBegun = baselineEnded = committed = activated = false;
    controlled = 0;
    clearInput();
    clearPrediction();
    interpolation.reset();
    std::vector<uint16_t>().swap(freeSlots);
    std::vector<Entity>().swap(predictedScratch);
    offset = glm::vec3(0.0f);
    offsetAt = -1e9;
}

// ---- Host -----------------------------------------------------------------------------------

bool NetworkWorld::Impl::captureState(NetObject& o) {
    if (!w.m_world || !o.profile) return false;
    ProfileValue value;
    ReplicationContext context;
    context.tickRate = settings().tickRate;
    const char* why = captureProfile(*o.profile, *w.m_world, o.entity, context, value);
    uint32_t written = 0;
    if (!why && !encodeProfile(*o.profile, value, o.state, written)) why = "it doesn't encode";
    if (why) {
        ++stats.captureFailures;
        if (!captureWarned)
            SW_LOG(LogNetwork, Warning, "Network object {} ({}) wasn't sent: {}", o.id, o.profile->name, why);
        captureWarned = true;
        return false;
    }
    o.stateBytes = uint16_t(written);
    return true;
}

void NetworkWorld::Impl::encodeSpawn(const NetObject& o, Spawn& spawn) const {
    spawn.controlSerial = std::max<uint32_t>(o.serial, 1);
    spawn.entity = o.id;
    spawn.incarnation = o.incarnation;
    spawn.ownerSlot = o.owner;
    spawn.prefabId = o.prefabId;
    spawn.authoredKey = o.prefabId == 0 ? std::string_view(o.key) : std::string_view();
    spawn.profileId = o.profile->id;
    spawn.initialState = std::span(o.state.data(), o.stateBytes);
}

// Appends an encoded message to the peer's control queue; false when it doesn't fit.
bool NetworkWorld::Impl::append(Peer& p, std::span<const uint8_t> bytes, uint32_t controlSerial) {
    if (p.queued() + kQueueEntryHeader + bytes.size() > p.queueCap) return false;
    if (p.queueRead > 0 && p.queueRead * 2 >= p.queue.size()) {
        p.queue.erase(p.queue.begin(), p.queue.begin() + std::ptrdiff_t(p.queueRead));
        p.queueRead = 0;
    }
    const size_t at = p.queue.size();
    p.queue.resize(at + kQueueEntryHeader + bytes.size());
    writeU32(p.queue.data() + at, uint32_t(bytes.size()));
    writeU32(p.queue.data() + at + 4, controlSerial);
    std::memcpy(p.queue.data() + at + kQueueEntryHeader, bytes.data(), bytes.size());
    stats.controlQueueHigh = std::max(stats.controlQueueHigh, uint32_t(p.queued()));
    return true;
}

bool NetworkWorld::Impl::enqueue(Peer& p, const MessageBody& body) {
    uint32_t written = 0;
    if (!encodeMessage(body, session, tick, controlOut, written)) return false;
    return append(p, std::span(controlOut.data(), written), serialOf(body));
}

// Every welcomed peer gets every lifecycle change after its baseline. A peer whose queue
// is full can't be kept consistent, so it's dropped (after the loop: rejecting sends).
void NetworkWorld::Impl::broadcast(const MessageBody& body) {
    uint32_t written = 0;
    if (!encodeMessage(body, session, tick, controlOut, written)) {
        SW_LOG(LogNetwork, Error, "A lifecycle message didn't encode");
        return;
    }
    const uint32_t controlSerial = serialOf(body);
    std::array<bool, kMaxTransportConnections> full{};
    for (size_t i = 0; i < peers.size(); ++i)
        if (peers[i].admitted()) full[i] = !append(peers[i], std::span(controlOut.data(), written), controlSerial);
    for (size_t i = 0; i < peers.size(); ++i)
        if (full[i] && peers[i].admitted()) reject(peers[i], ReasonCode::RateLimited, "fell too far behind the host");
}

void NetworkWorld::Impl::startBaseline(Peer& p) {
    p.queue.clear();
    p.queueRead = 0;
    p.sentSerial = 0;
    p.baselineId = 1;
    uint32_t stateBytes = 0;
    size_t estimate = 2 * (kHeaderBytes + 64) + 2 * kQueueEntryHeader;
    for (const NetObject& o : objects) {
        stateBytes += o.stateBytes;
        estimate += kHeaderBytes + 40 + o.key.size() + o.stateBytes + kQueueEntryHeader;
    }
    p.queueCap = estimate + std::max<size_t>(65536, settings().maxPendingReliableBytes);
    p.queue.reserve(estimate);
    BaselineBegin begin;
    begin.baselineId = p.baselineId;
    begin.baselineTick = tick;
    begin.objectCount = uint16_t(objects.size());
    begin.totalStateBytes = stateBytes;
    bool ok = enqueue(p, begin);
    for (const NetObject& o : objects) {
        Spawn spawn;
        encodeSpawn(o, spawn);
        ok = ok && enqueue(p, spawn);
    }
    BaselineEnd end;
    end.baselineId = p.baselineId;
    end.finalControlSerial = serial;
    end.baselineDigest = catalogDigest();
    ok = ok && enqueue(p, end);
    if (!ok) return reject(p, ReasonCode::ServiceUnavailable, "the baseline didn't fit");
    p.baselineDigest = end.baselineDigest;
    ++stats.baselinesSent;
    stats.baselineBytes += p.queued();
    flushControl(p);
}

void NetworkWorld::Impl::ready(Peer& p, const Ready& ready) {
    if (p.phase != Peer::Phase::Synchronizing) return reject(p, ReasonCode::MalformedPacket, "Ready twice");
    if (ready.baselineId != p.baselineId || ready.baselineDigest != p.baselineDigest)
        return reject(p, ReasonCode::MalformedPacket, "the baseline didn't match");
    Active active;
    active.startTick = tick;
    active.firstInputTick = tick + 1;
    for (const NetObject& o : objects)
        if (o.owner == p.slot) {
            active.controlledEntity = o.id;
            active.incarnation = o.incarnation;
            break;
        }
    // After every lifecycle change still queued, so the client has them all when it starts.
    if (!enqueue(p, active)) return reject(p, ReasonCode::RateLimited, "fell too far behind the host");
    enter(p, Peer::Phase::Active);
    p.firstInputTick = active.firstInputTick;   // its inputs count from here
    p.fullRefresh = true;
    SW_LOG(LogNetwork, Info, "Player in slot {} is in the game", p.slot);
    flushControl(p);
}

void NetworkWorld::Impl::resyncRequest(Peer& p) {
    if (p.phase != Peer::Phase::Active) return reject(p, ReasonCode::MalformedPacket, "ResyncRequest before Active");
    ++stats.resyncRequests;
    auto& times = p.resyncs;   // oldest first
    // The client spaces its requests by its own clock; network jitter can bring two of
    // them closer here, so the host allows half the interval and 55 s for the minute.
    if (now - times.back() < kResyncIntervalSeconds * 0.5 || now - times.front() < 55)
        return reject(p, ReasonCode::RateLimited, "too many resync requests");
    std::rotate(times.begin(), times.begin() + 1, times.end());
    times.back() = now;
    p.fullRefresh = true;
}

void NetworkWorld::Impl::changeOwner(NetObject& o, uint16_t slot) {
    o.owner = slot;
    ++o.incarnation;
    o.serial = ++serial;
    writeIdentity(o);
    Ownership message;
    message.controlSerial = o.serial;
    message.entity = o.id;
    message.incarnation = o.incarnation;
    message.ownerSlot = slot;
    ++stats.ownershipChanges;
    broadcast(message);
}

// A player left: nobody owns their objects until the game gives them to someone.
void NetworkWorld::Impl::ownerLeft(uint16_t slot) {
    if (slot == 0) return;
    for (size_t i = 0; i < objects.size(); ++i)
        if (objects[i].owner == slot) changeOwner(objects[i], 0);
}

void NetworkWorld::Impl::despawnObject(uint64_t id) {
    NetObject* o = object(id);
    if (!o) return;
    Despawn message;
    message.controlSerial = ++serial;
    message.entity = id;
    if (w.m_world && w.m_world->alive(o->entity)) destroyWithChildren(*w.m_world, o->entity);
    erase(id);
    ++stats.despawns;
    broadcast(message);
}

// Sends what the budget allows from the peer's control queue, oldest first.
void NetworkWorld::Impl::flushControl(Peer& p) {
    if (!p.admitted() || !w.m_transport) return;
    const double rate = double(settings().outboundBytesPerSecond);
    if (p.outTokensAt >= 0) p.outTokens = std::min(kOutboundBurstBytes, p.outTokens + (now - p.outTokensAt) * rate);
    p.outTokensAt = now;
    const uint32_t pendingLimit = std::max<uint32_t>(settings().maxPendingReliableBytes, kMaxReliableMessageBytes);
    while (p.queued() > 0) {
        const uint8_t* entry = p.queue.data() + p.queueRead;
        const uint32_t bytes = readU32(entry);
        const uint32_t controlSerial = readU32(entry + 4);
        if (p.outTokens < double(bytes)) break;
        if (auto s = w.m_transport->stats(p.connection); s && s->pendingReliableBytes + bytes > pendingLimit) break;
        const SendResult result =
            w.m_transport->send(p.connection, std::span(entry + kQueueEntryHeader, bytes), Delivery::ReliableOrdered);
        if (result == SendResult::Backpressure) break;
        if (result != SendResult::Queued) return;   // the connection is going; its Closed event follows
        p.outTokens -= double(bytes);
        p.queueRead += kQueueEntryHeader + bytes;
        if (controlSerial && newer(controlSerial, p.sentSerial)) p.sentSerial = controlSerial;
        ++stats.messagesSent;
        stats.controlBytesSent += bytes;
    }
    if (p.queueRead == p.queue.size()) {
        p.queue.clear();
        p.queueRead = 0;
    }
}

void NetworkWorld::Impl::flushAll() {
    for (Peer& p : peers)
        if (p.admitted()) flushControl(p);
}

// Objects game code destroyed without despawn(): gone everywhere at the end of the tick.
void NetworkWorld::Impl::sweepDestroyed() {
    if (!w.m_world) return;
    for (size_t i = 0; i < objects.size();) {
        if (w.m_world->alive(objects[i].entity)) {
            ++i;
            continue;
        }
        despawnObject(objects[i].id);   // erases objects[i]
    }
}

void NetworkWorld::Impl::sendSnapshots() {
    const uint32_t every = std::max<uint32_t>(1, settings().tickRate / std::max<uint32_t>(1, settings().snapshotRate));
    const bool due = tick % every == 0;
    bool anyone = false;
    for (const Peer& p : peers) anyone = anyone || (p.phase == Peer::Phase::Active && (due || p.fullRefresh));
    if (!anyone || objects.empty()) return;
    for (NetObject& o : objects)
        if (!o.isStatic || o.changed) captureState(o);   // a failure keeps the last good state
    ++snapshotId;
    for (Peer& p : peers)
        if (p.phase == Peer::Phase::Active && (due || p.fullRefresh)) sendSnapshot(p);
    if (due)
        for (NetObject& o : objects) o.changed = false;
}

// One Snapshot for one peer: its players, then other moving objects round-robin, as far
// as the budget goes, split into parts of at most max_snapshot_bytes. A record is only
// sent once the lifecycle change it depends on has gone out to this peer.
void NetworkWorld::Impl::sendSnapshot(Peer& p) {
    const NetworkSettings& s = settings();
    const double rate = double(s.outboundBytesPerSecond);
    if (p.outTokensAt >= 0) p.outTokens = std::min(kOutboundBurstBytes, p.outTokens + (now - p.outTokensAt) * rate);
    p.outTokensAt = now;
    const double reserve = std::min(double(kReservedControlBytesPerSecond), rate / 2);
    const bool full = p.fullRefresh;
    double budget = full ? 1e18 : std::min((rate - reserve) / std::max<uint32_t>(1, s.snapshotRate), p.outTokens);
    const uint32_t partLimit = std::min<uint32_t>(
        std::max<uint32_t>(s.maxSnapshotBytes, kSnapshotOverheadBytes + 69), kMaxUnreliableMessageBytes);
    const uint32_t partRoom = partLimit - kSnapshotOverheadBytes;
    pick.clear();
    partEnds.clear();
    uint32_t partUsed = 0;
    double spent = 0;
    // Adds a record if it fits the budget and the 256 parts; false when it didn't.
    auto take = [&](uint32_t index) {
        const NetObject& o = objects[index];
        const uint32_t bytes = kStateRecordHeaderBytes + o.stateBytes;
        const bool newPart = partEnds.empty() || partUsed + bytes > partRoom;
        const double cost = bytes + (newPart ? kSnapshotOverheadBytes : 0);
        if (spent + cost > budget || (newPart && partEnds.size() == 256)) return false;
        if (newPart) {
            partEnds.push_back(uint32_t(pick.size()));
            partUsed = 0;
        }
        pick.push_back(index);
        partUsed += bytes;
        spent += cost;
        partEnds.back() = uint32_t(pick.size());
        return true;
    };
    auto sendable = [&](const NetObject& o) {
        return o.stateBytes > 0 && !newer(o.serial, p.sentSerial) && (full || !o.isStatic || o.changed);
    };
    auto player = [](const NetObject& o) { return o.owner != 0 || o.profile->perPlayer; };
    for (uint32_t i = 0; i < objects.size(); ++i)
        if (sendable(objects[i]) && player(objects[i]) && !take(i)) ++stats.recordsLimited;
    const uint32_t n = uint32_t(objects.size());
    uint32_t start = p.propCursor % std::max<uint32_t>(n, 1), next = start;
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t i = (start + k) % n;
        if (!sendable(objects[i]) || player(objects[i])) continue;
        if (!take(i)) {
            ++stats.recordsLimited;
            continue;
        }
        next = (i + 1) % n;
    }
    p.propCursor = next;
    p.fullRefresh = false;
    if (pick.empty()) return;
    uint32_t first = 0;
    for (uint32_t partIndex = 0; partIndex < partEnds.size(); ++partIndex) {
        const uint32_t end = partEnds[partIndex];
        WireWriter writer(part);
        uint32_t required = 0;
        for (uint32_t k = first; k < end; ++k) {
            const NetObject& o = objects[pick[k]];
            StateRecord record;
            record.entity = o.id;
            record.incarnation = o.incarnation;
            record.profileId = o.profile->id;
            record.state = std::span(o.state.data(), o.stateBytes);
            (void)writeStateRecord(writer, record);
            if (newer(o.serial, required)) required = o.serial;
        }
        Snapshot snapshot;
        snapshot.snapshotId = snapshotId;
        snapshot.serverTick = tick;
        snapshot.requiredControlSerial = required;
        snapshot.partIndex = uint16_t(partIndex);
        snapshot.partCount = uint16_t(partEnds.size());
        snapshot.recordCount = uint16_t(end - first);
        snapshot.records = writer.written();
        uint32_t written = 0;
        if (writer.ok() && encodeMessage(snapshot, session, tick, out, written)) {
            const SendResult result =
                w.m_transport->send(p.connection, std::span(out.data(), written), Delivery::Unreliable);
            if (result == SendResult::Queued) {
                ++stats.messagesSent;
                ++stats.snapshotPartsSent;
                stats.snapshotBytesSent += written;
                stats.recordsSent += end - first;
            }
            p.outTokens -= double(written);
        }
        first = end;
    }
    ++stats.snapshotsSent;
}

// ---- Client ---------------------------------------------------------------------------------

namespace {

// Fails the session; returns true so a handler can `return bad(...)` for a message it took.
#define SW_NET_BAD(reason, why)                                                                                        \
    do {                                                                                                               \
        failClient(reason, why);                                                                                       \
        return true;                                                                                                   \
    } while (0)

}   // namespace

bool NetworkWorld::Impl::validState(const ReplicationProfile& profile, std::span<const uint8_t> bytes) {
    if (bytes.size() != profile.encodedBytes) return false;
    ProfileValue value;
    if (!decodeProfile(profile, bytes, value)) return false;
    return !profile.validate || !profile.validate(value);
}

bool NetworkWorld::Impl::checkSerial(uint32_t controlSerial) {
    if (!baselineEnded) return true;   // the baseline's own Spawns carry older serials
    if (controlSerial != serial + 1) {
        failClient(ReasonCode::MalformedPacket, "lifecycle messages arrived out of control-serial order");
        return false;
    }
    serial = controlSerial;
    return true;
}

bool NetworkWorld::Impl::replicationMessage(const DecodedMessage& decoded) {
    const NetworkSettings& s = settings();
    switch (kindOf(decoded.body)) {
    case MessageKind::BaselineBegin: {
        const auto& m = std::get<BaselineBegin>(decoded.body);
        if (baselineBegun) SW_NET_BAD(ReasonCode::MalformedPacket, "BaselineBegin twice");
        if (m.baselineId != baselineId) SW_NET_BAD(ReasonCode::MalformedPacket, "a baseline Welcome didn't announce");
        if (m.objectCount > maxEntities) SW_NET_BAD(ReasonCode::MalformedPacket, "a baseline over max_entities");
        baselineBegun = true;
        baselineCount = m.objectCount;
        return true;
    }
    case MessageKind::Spawn: return spawnMessage(std::get<Spawn>(decoded.body));
    case MessageKind::Despawn: {
        const auto& m = std::get<Despawn>(decoded.body);
        if (!baselineEnded) SW_NET_BAD(ReasonCode::MalformedPacket, "Despawn inside the baseline");
        if (!checkSerial(m.controlSerial)) return true;
        NetObject* o = object(m.entity);
        if (!o) SW_NET_BAD(ReasonCode::MalformedPacket, "Despawn for an object that isn't there");
        if (committed && w.m_world && w.m_world->alive(o->entity)) destroyWithChildren(*w.m_world, o->entity);
        erase(m.entity);
        ++stats.despawns;
        releaseHeld();
        return true;
    }
    case MessageKind::Ownership: {
        const auto& m = std::get<Ownership>(decoded.body);
        if (!baselineEnded) SW_NET_BAD(ReasonCode::MalformedPacket, "Ownership inside the baseline");
        if (!checkSerial(m.controlSerial)) return true;
        NetObject* o = object(m.entity);
        if (!o) SW_NET_BAD(ReasonCode::MalformedPacket, "Ownership for an object that isn't there");
        if (!newer(m.incarnation, o->incarnation))
            SW_NET_BAD(ReasonCode::MalformedPacket, "Ownership without a newer incarnation");
        if (m.ownerSlot > s.maxPlayers) SW_NET_BAD(ReasonCode::MalformedPacket, "an owner slot over max_players");
        o->owner = m.ownerSlot;
        o->incarnation = m.incarnation;
        o->serial = m.controlSerial;
        o->haveTick = false;
        if (committed) writeIdentity(*o);
        ++stats.ownershipChanges;
        releaseHeld();
        return true;
    }
    case MessageKind::InitialState: {
        const auto& m = std::get<InitialState>(decoded.body);
        if (!baselineBegun || m.baselineId != baselineId)
            SW_NET_BAD(ReasonCode::MalformedPacket, "InitialState outside its baseline");
        StateRecordReader reader(m.records, m.recordCount);
        StateRecord r;
        while (reader.next(r)) {
            NetObject* o = object(r.entity);
            if (!o || o->incarnation != r.incarnation || o->profile->id != r.profileId)
                SW_NET_BAD(ReasonCode::MalformedPacket, "InitialState for an object the baseline doesn't have");
            if (!validState(*o->profile, r.state))
                SW_NET_BAD(ReasonCode::MalformedPacket, "InitialState outside its profile's ranges");
            std::memcpy(o->state.data(), r.state.data(), r.state.size());
            o->stateBytes = uint16_t(r.state.size());
            if (committed) applyState(*o);
        }
        return true;
    }
    case MessageKind::BaselineEnd: {
        const auto& m = std::get<BaselineEnd>(decoded.body);
        if (!baselineBegun || baselineEnded || m.baselineId != baselineId)
            SW_NET_BAD(ReasonCode::MalformedPacket, "BaselineEnd without its BaselineBegin");
        if (objects.size() != baselineCount)
            SW_NET_BAD(ReasonCode::MalformedPacket, "the baseline had a different number of objects than it said");
        for (const NetObject& o : objects)
            if (newer(o.serial, m.finalControlSerial))
                SW_NET_BAD(ReasonCode::MalformedPacket, "a baseline object newer than the baseline");
        const Digest digest = catalogDigest();
        if (digest != m.baselineDigest) SW_NET_BAD(ReasonCode::MalformedPacket, "the baseline's digest didn't match");
        baselineDigest = digest;
        serial = m.finalControlSerial;
        baselineEnded = true;
        tryCommit();
        return true;
    }
    case MessageKind::Active: {
        const auto& m = std::get<Active>(decoded.body);
        if (!committed || activated) SW_NET_BAD(ReasonCode::MalformedPacket, "Active before Ready");
        if (m.controlledEntity != 0) {
            const NetObject* o = object(m.controlledEntity);
            if (!o || o->owner != w.m_localSlot)
                SW_NET_BAD(ReasonCode::MalformedPacket, "Active named an object this player doesn't own");
        }
        activated = true;
        controlled = m.controlledEntity;
        startInterpolation();
        firstInputTick = m.firstInputTick;
        nextPing = now;   // start measuring the host's clock
        SW_LOG(LogNetwork, Info, "In the game as player {}, {} network objects", w.m_localSlot, objects.size());
        w.setState(NetworkState::Active);
        return true;
    }
    default: return false;
    }
}

bool NetworkWorld::Impl::spawnMessage(const Spawn& m) {
    const NetworkSettings& s = settings();
    if (!baselineBegun) SW_NET_BAD(ReasonCode::MalformedPacket, "Spawn before the baseline");
    const ReplicationRegistry* registry = w.m_start.registry;
    const ReplicationProfile* profile = registry ? registry->profile(m.profileId) : nullptr;
    if (!profile) SW_NET_BAD(ReasonCode::ContentMismatch, "Spawn with a profile this build doesn't have");
    if (!validState(*profile, m.initialState))
        SW_NET_BAD(ReasonCode::MalformedPacket, "Spawn state outside its profile's ranges");
    if (m.ownerSlot > s.maxPlayers) SW_NET_BAD(ReasonCode::MalformedPacket, "an owner slot over max_players");
    if (m.prefabId == 0) {
        auto it = localKeys.find(std::string(m.authoredKey));
        if (it == localKeys.end())
            SW_NET_BAD(ReasonCode::ContentMismatch, "Spawn for an authored key this scene lacks");
        const NetworkObject* n = w.m_world ? w.m_world->get<NetworkObject>(it->second) : nullptr;
        if (!n || n->profile != profile->name)
            SW_NET_BAD(ReasonCode::ContentMismatch, "an authored object's profile differs from the host's");
    } else {
        const NetworkPrefab* prefab = findPrefab(w.m_start, m.prefabId);
        if (!prefab) SW_NET_BAD(ReasonCode::MalformedPacket, "Spawn of a prefab that isn't in the network manifest");
        if (prefab->profile != profile->name)
            SW_NET_BAD(ReasonCode::MalformedPacket, "Spawn with a profile its prefab doesn't have");
    }
    auto fill = [&](NetObject& o) {
        o.incarnation = m.incarnation;
        o.owner = m.ownerSlot;
        o.serial = m.controlSerial;
        o.haveTick = false;
        std::memcpy(o.state.data(), m.initialState.data(), m.initialState.size());
        o.stateBytes = uint16_t(m.initialState.size());
    };
    NetObject* existing = object(m.entity);
    if (!baselineEnded) {   // staged: nothing touches the world before the whole baseline checks out
        if (existing) SW_NET_BAD(ReasonCode::MalformedPacket, "an object twice in the baseline");
        if (objects.size() >= baselineCount)
            SW_NET_BAD(ReasonCode::MalformedPacket, "more baseline objects than BaselineBegin said");
        if (!objects.empty() && m.entity <= objects.back().id)
            SW_NET_BAD(ReasonCode::MalformedPacket, "baseline objects out of ID order");
        if (m.prefabId == 0)
            for (const NetObject& o : objects)
                if (o.prefabId == 0 && o.key == m.authoredKey)
                    SW_NET_BAD(ReasonCode::MalformedPacket, "two baseline objects with one key");
        NetObject o;
        o.id = m.entity;
        o.prefabId = m.prefabId;
        o.key = m.authoredKey;
        o.profile = profile;
        fill(o);
        highestId = m.entity;
        insert(std::move(o));
        return true;
    }
    if (!checkSerial(m.controlSerial)) return true;
    if (existing) {   // a respawn or teleport: the same object, a new incarnation
        if (!newer(m.incarnation, existing->incarnation))
            SW_NET_BAD(ReasonCode::MalformedPacket, "Spawn for a live object without a newer incarnation");
        if (existing->prefabId != m.prefabId || existing->profile != profile ||
            (m.prefabId == 0 && existing->key != m.authoredKey))
            SW_NET_BAD(ReasonCode::MalformedPacket, "a respawn changed what the object is");
        fill(*existing);
        if (committed) {
            writeIdentity(*existing);
            applyState(*existing);
            newLife(*existing);
        }
        ++stats.respawns;
        releaseHeld();
        return true;
    }
    if (m.prefabId == 0) SW_NET_BAD(ReasonCode::MalformedPacket, "an authored object outside the baseline");
    if (m.entity <= highestId) SW_NET_BAD(ReasonCode::MalformedPacket, "Spawn reused a despawned network ID");
    if (objects.size() >= maxEntities) SW_NET_BAD(ReasonCode::MalformedPacket, "more objects than max_entities");
    NetObject o;
    o.id = m.entity;
    o.prefabId = m.prefabId;
    o.profile = profile;
    fill(o);
    highestId = m.entity;
    insert(std::move(o));
    ++stats.spawns;
    if (committed && !materialize(objects.back())) return true;
    releaseHeld();
    return true;
}

void NetworkWorld::Impl::tryCommit() {
    if (committed || !baselineEnded || w.m_state != NetworkState::Synchronizing) return;
    if (!commitBaseline()) return;
    committed = true;
    Ready ready;
    ready.baselineId = baselineId;
    ready.baselineDigest = baselineDigest;
    if (!send(peers[0].connection, ready)) failClient(ReasonCode::ServiceUnavailable, "couldn't send Ready");
}

// Turns the checked catalog into entities: authored objects bound by key, prefabs spawned.
bool NetworkWorld::Impl::commitBaseline() {
    World* world = w.m_world;
    if (!world) {
        if (objects.empty()) return true;
        failClient(ReasonCode::ServiceUnavailable, "no world to replicate into");
        return false;
    }
    for (NetObject& o : objects)
        if (!materialize(o)) return false;
    // Authored objects the host didn't list were despawned before we joined.
    for (const auto& [key, e] : localKeys)
        if (world->alive(e) && !world->has<NetworkIdentity>(e)) destroyWithChildren(*world, e);
    return true;
}

bool NetworkWorld::Impl::materialize(NetObject& o) {
    World& world = *w.m_world;
    Entity e = kNullEntity;
    if (o.prefabId == 0) {
        auto it = localKeys.find(o.key);
        if (it != localKeys.end()) e = it->second;
        if (!world.alive(e)) {
            failClient(ReasonCode::ContentMismatch, "an authored network object is gone from the scene");
            return false;
        }
    } else {
        const NetworkPrefab* prefab = findPrefab(w.m_start, o.prefabId);
        if (prefab) e = spawnCopy(w.m_start, world, *prefab, Transform{});
        if (!world.alive(e)) {
            failClient(ReasonCode::ContentMismatch, "couldn't spawn a network prefab");
            return false;
        }
        NetworkObject* n = world.get<NetworkObject>(e);
        if (!n) n = &world.add<NetworkObject>(e);
        n->profile = o.profile->name;
        n->prefab = prefab->path;
    }
    o.entity = e;
    writeIdentity(o);
    // The host simulates it; here physics (in Client mode) keeps it as a proxy that
    // follows the snapshots, or, for the controlled character, moves it by prediction.
    applyState(o);
    newLife(o);
    return true;
}

void NetworkWorld::Impl::applyState(NetObject& o) {
    World* world = w.m_world;
    if (!world || !world->alive(o.entity) || !o.profile->apply) return;
    ProfileValue value;
    if (!decodeProfile(*o.profile, std::span(o.state.data(), o.stateBytes), value)) return;
    ReplicationContext context;
    context.tickRate = settings().tickRate;
    o.profile->apply(*world, o.entity, context, value);
}

// Records apply one by one: each object keeps its own newest tick, so a reordered or
// duplicate part changes nothing, and a lost part holds nothing else back.
bool NetworkWorld::Impl::applySnapshot(const Snapshot& snapshot) {
    StateRecordReader reader(snapshot.records, snapshot.recordCount);
    StateRecord r;
    while (reader.next(r)) {
        NetObject* o = object(r.entity);
        if (!o) {
            if (r.entity <= highestId) {   // despawned since: IDs are never reused
                ++stats.staleRecords;
                continue;
            }
            failClient(ReasonCode::MalformedPacket, "a snapshot named an object the host never spawned");
            return false;
        }
        if (r.incarnation != o->incarnation) {
            if (newer(r.incarnation, o->incarnation)) {
                failClient(ReasonCode::MalformedPacket, "a snapshot ahead of its object's incarnation");
                return false;
            }
            ++stats.staleRecords;   // the object's previous life
            continue;
        }
        if (r.profileId != o->profile->id) {
            failClient(ReasonCode::MalformedPacket, "a snapshot record with the wrong profile");
            return false;
        }
        if (o->haveTick && !newer(snapshot.serverTick, o->lastTick)) {
            ++stats.staleRecords;
            continue;
        }
        if (!validState(*o->profile, r.state)) {
            failClient(ReasonCode::MalformedPacket, "snapshot state outside its profile's ranges");
            return false;
        }
        std::memcpy(o->state.data(), r.state.data(), r.state.size());
        o->stateBytes = uint16_t(r.state.size());
        o->lastTick = snapshot.serverTick;
        o->haveTick = true;
        if (!haveApplied || newer(snapshot.serverTick, lastAppliedTick)) lastAppliedTick = snapshot.serverTick;
        haveApplied = true;
        lastSnapshotAt = now;
        if (predictsControlled() && o->entity == controlledEntityNow())
            applyPredicted(*o, snapshot.serverTick);
        else {
            applyState(*o);
            pushInterpolation(*o, snapshot.serverTick);
        }
        ++stats.recordsApplied;
    }
    if (interpolation && snapshot.recordCount) interpolation->observeServerTick(snapshot.serverTick, now);
    return true;
}

void NetworkWorld::Impl::holdSnapshot(std::span<const uint8_t> bytes, uint32_t requiredSerial) {
    HeldSnapshot* slot = nullptr;
    for (HeldSnapshot& h : held)
        if (!h.used) {
            slot = &h;
            break;
        }
    if (!slot) {   // full: the oldest goes
        slot = &held[0];
        for (HeldSnapshot& h : held)
            if (h.at < slot->at) slot = &h;
        ++stats.expiredSnapshots;
        requestResync(kResyncSnapshotExpired);
    }
    slot->used = true;
    slot->at = now;
    slot->requiredSerial = requiredSerial;
    slot->bytes = uint32_t(bytes.size());
    std::memcpy(slot->data.data(), bytes.data(), bytes.size());
    ++stats.heldSnapshots;
}

// Held snapshots whose control serial has now arrived, oldest first.
void NetworkWorld::Impl::releaseHeld() {
    while (!failPending) {
        HeldSnapshot* next = nullptr;
        for (HeldSnapshot& h : held)
            if (h.used && !newer(h.requiredSerial, serial) && (!next || h.at < next->at)) next = &h;
        if (!next) return;
        next->used = false;
        DecodeOptions options;
        options.delivery = Delivery::Unreliable;
        options.receiver = PeerRole::Client;
        DecodedMessage decoded;
        if (decodeMessage(std::span(next->data.data(), next->bytes), options, decoded))
            applySnapshot(std::get<Snapshot>(decoded.body));
    }
}

void NetworkWorld::Impl::expireHeld() {
    for (HeldSnapshot& h : held)
        if (h.used && now - h.at > kHoldSeconds) {
            h.used = false;
            ++stats.expiredSnapshots;
            requestResync(kResyncSnapshotExpired);
        }
}

void NetworkWorld::Impl::requestResync(uint8_t reason) {
    if (now - lastResync < kResyncIntervalSeconds || now - resyncTimes.front() < 60) return;
    lastResync = now;
    std::rotate(resyncTimes.begin(), resyncTimes.begin() + 1, resyncTimes.end());
    resyncTimes.back() = now;
    ResyncRequest request;
    request.lastAppliedServerTick = lastAppliedTick;
    request.reason = reason;
    if (send(peers[0].connection, request)) ++stats.resyncRequests;
}

Digest NetworkWorld::Impl::catalogDigest() const {
    Sha256 hash;
    hash.update(std::string_view("shockwave_baseline 1\n"));
    hash.updateU32(uint32_t(objects.size()));
    for (const NetObject& o : objects) {
        hash.updateU64(o.id);
        hash.updateU32(o.incarnation);
        hash.updateU16(o.owner);
        hash.updateU16(o.prefabId);
        const std::string_view key = o.prefabId == 0 ? std::string_view(o.key) : std::string_view();
        hash.updateU16(uint16_t(key.size()));
        hash.update(key);
        hash.updateU16(o.profile->id);
        hash.updateU16(o.stateBytes);
        hash.update(std::span(o.state.data(), o.stateBytes));
    }
    return hash.finish();
}

#undef SW_NET_BAD

// ---- NetworkWorld facade --------------------------------------------------------------------

const NetworkIdentity* NetworkWorld::identity(Entity entity) const {
    return m_world ? m_world->get<NetworkIdentity>(entity) : nullptr;
}

uint64_t NetworkWorld::networkId(Entity entity) const {
    const NetworkIdentity* id = identity(entity);
    return id ? id->id : 0;
}

Entity NetworkWorld::entity(uint64_t networkId) const {
    NetObject* o = m_impl->object(networkId);
    return o && m_world && m_world->alive(o->entity) ? o->entity : kNullEntity;
}

bool NetworkWorld::isLocallyControlled(Entity entity) const {
    if (!online()) return true;
    const NetworkIdentity* id = identity(entity);
    return id && id->ownerSlot != 0 && id->ownerSlot == m_localSlot;
}

Entity NetworkWorld::controlledEntity() const { return m_impl->controlled ? entity(m_impl->controlled) : kNullEntity; }

uint32_t NetworkWorld::objectCount() const { return uint32_t(m_impl->objects.size()); }

namespace {

NetError notHost() { return {NetErrorCode::InvalidState, "only the host (or offline play) changes network objects"}; }

}   // namespace

NetResult<Entity> NetworkWorld::spawn(uint16_t prefabId, const Transform& at, uint16_t ownerSlot) {
    if (!authority()) return notHost();
    if (!m_world) return NetError{NetErrorCode::InvalidState, "the session has no world"};
    const NetworkPrefab* prefab = findPrefab(m_start, prefabId);
    if (!prefab) return NetError{NetErrorCode::InvalidArgument, "that prefab isn't in the network manifest"};
    const bool hosting = online() && !m_closed && m_transport;
    if (online() && !hosting) return NetError{NetErrorCode::InvalidState, "the session isn't hosting"};
    if (ownerSlot > m_start.settings.maxPlayers) return NetError{NetErrorCode::InvalidArgument, "no such player slot"};
    Impl& impl = *m_impl;
    const ReplicationProfile* profile = m_start.registry ? m_start.registry->profile(prefab->profile) : nullptr;
    if (hosting) {
        if (!profile) return NetError{NetErrorCode::InvalidArgument, "the prefab's profile isn't registered"};
        if (impl.objects.size() >= std::clamp<uint32_t>(m_start.settings.maxReplicatedEntities, 12, kMaxNetworkObjects))
            return NetError{NetErrorCode::Unavailable, "max_replicated_entities network objects already exist"};
        if (impl.nextId > kMaxSpawnsPerSession)
            return NetError{NetErrorCode::Unavailable, "this session has used up its network IDs"};
    }
    const Entity e = spawnCopy(m_start, *m_world, *prefab, at);
    if (!m_world->alive(e)) return NetError{NetErrorCode::Io, "couldn't spawn the prefab"};
    if (!hosting) return e;   // offline: a plain copy
    NetworkObject* n = m_world->get<NetworkObject>(e);
    if (!n) n = &m_world->add<NetworkObject>(e);
    n->profile = prefab->profile;
    n->prefab = prefab->path;
    NetObject o;
    o.id = impl.nextId++;
    o.owner = ownerSlot;
    o.prefabId = prefabId;
    o.profile = profile;
    o.entity = e;
    o.isStatic = n->mode == NetworkObject::Mode::Static;
    if (!impl.captureState(o)) {
        destroyWithChildren(*m_world, e);
        return NetError{NetErrorCode::InvalidArgument, "the new object's state is outside its profile's ranges"};
    }
    o.serial = ++impl.serial;
    impl.writeIdentity(o);
    Spawn message;
    impl.encodeSpawn(o, message);
    impl.insert(std::move(o));
    ++impl.stats.spawns;
    impl.broadcast(message);
    return e;
}

NetResult<void> NetworkWorld::despawn(Entity entity) {
    if (!authority()) return notHost();
    if (!m_world || !m_world->alive(entity)) return NetError{NetErrorCode::InvalidArgument, "no such entity"};
    if (!online()) {
        destroyWithChildren(*m_world, entity);
        return {};
    }
    NetObject* o = m_impl->objectOf(entity);
    if (!o) return NetError{NetErrorCode::InvalidArgument, "not a network object"};
    m_impl->despawnObject(o->id);
    return {};
}

NetResult<void> NetworkWorld::respawn(Entity entity, const Transform* at) {
    if (!authority()) return notHost();
    if (!m_world || !m_world->alive(entity)) return NetError{NetErrorCode::InvalidArgument, "no such entity"};
    if (at)
        if (Transform* t = m_world->get<Transform>(entity)) {
            t->position = at->position;
            t->rotation = at->rotation;
        }
    if (!online()) return {};
    Impl& impl = *m_impl;
    NetObject* o = impl.objectOf(entity);
    if (!o) return NetError{NetErrorCode::InvalidArgument, "not a network object"};
    if (!impl.captureState(*o))
        return NetError{NetErrorCode::InvalidArgument, "its state is outside its profile's ranges"};
    ++o->incarnation;
    o->serial = ++impl.serial;
    impl.writeIdentity(*o);
    Spawn message;
    impl.encodeSpawn(*o, message);
    ++impl.stats.respawns;
    impl.broadcast(message);
    return {};
}

NetResult<void> NetworkWorld::setOwner(Entity entity, uint16_t ownerSlot) {
    if (!authority()) return notHost();
    if (ownerSlot > m_start.settings.maxPlayers) return NetError{NetErrorCode::InvalidArgument, "no such player slot"};
    if (!online()) return {};
    NetObject* o = m_impl->objectOf(entity);
    if (!o) return NetError{NetErrorCode::InvalidArgument, "not a network object"};
    m_impl->changeOwner(*o, ownerSlot);
    return {};
}

void NetworkWorld::markChanged(Entity entity) {
    if (NetObject* o = authority() ? m_impl->objectOf(entity) : nullptr) o->changed = true;
}

}   // namespace shockwave::net
