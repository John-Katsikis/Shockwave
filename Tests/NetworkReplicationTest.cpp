// Replication without prediction (plan Batch 5, docs/Networking.md "Replication"):
// NetworkWorlds with real Worlds over an in-process LoopbackHub. A host and clients whose
// scenes were built in different orders (authored objects bind by key), allowlisted
// spawns and despawns, late join, respawn incarnations, owners, the per-peer budget, a
// baseline torn down halfway, and a scripted host that sends what a real one wouldn't:
// snapshot parts reversed and duplicated, a despawn before an old snapshot, snapshots
// ahead of their control serial, and malformed authority data. No Steam, sockets,
// window or physics engine.
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Multiplayer/NetworkComponents.h>
#include <Multiplayer/NetworkWorld.h>
#include <Multiplayer/Replication.h>
#include <Network/Loopback.h>
#include <Network/LoopbackServices.h>
#include <Network/Sha256.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>

using namespace shockwave;
using namespace shockwave::net;

static std::atomic<uint64_t> g_allocations{0};
void* operator new(std::size_t size) {
    ++g_allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    ++g_allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

int failures = 0, checks = 0;
void check(bool value, const std::string& message) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

const ReplicationRegistry& registry() {
    static const ReplicationRegistry frozen = [] {
        ReplicationRegistry r;
        registerEngineReplication(r);
        std::vector<std::string> problems;
        r.freeze(problems);
        return r;
    }();
    return frozen;
}

constexpr uint16_t kCrate = 1;
constexpr TransportAddress kAddress{TransportAddress::Loopback, 1, 0};

Entity makeCrate(World& world, const NetworkPrefab& prefab, const Transform& at) {
    const Entity e = world.create();
    world.add<Transform>(e, at);
    world.add<Name>(e, {"crate"});
    NetworkObject n;
    n.profile = prefab.profile;
    world.add<NetworkObject>(e, n);
    return e;
}

Entity addAuthored(World& world, const char* key, glm::vec3 at = {}) {
    const Entity e = world.create();
    world.add<Name>(e, {key});
    world.add<Transform>(e, {at});
    NetworkObject n;
    n.key = key;
    n.profile = "engine.pose.v1";
    world.add<NetworkObject>(e, n);
    return e;
}

// Builds the same authored objects in a different order, with unrelated entities first.
void makeScene(World& world, int order) {
    for (int i = 0; i < order; ++i) world.add<Name>(world.create(), {"filler"});
    const char* keys[] = {"alpha", "bravo", "charlie"};
    for (int i = 0; i < 3; ++i) addAuthored(world, keys[(i + order) % 3], {float(i), 0, 0});
}

Entity byKey(World& world, const char* key) {
    Entity found = kNullEntity;
    world.each<const NetworkObject>([&](Entity e, const NetworkObject& n) {
        if (n.key == key) found = e;
    });
    return found;
}

NetworkStart startFor(NetworkRole role) {
    NetworkStart s;
    s.role = role;
    s.settings.tickRate = 60;
    s.settings.snapshotRate = 20;
    s.settings.maxPlayers = 8;
    s.compatibility.gameProtocol = "replication-test";
    s.compatibility.schemaDigest = registry().schemaDigest();
    s.address = kAddress;
    s.authenticate = false;   // loopback only; the handshake tests cover tickets
    s.registry = &registry();
    s.prefabs = {{kCrate, "Prefabs/Crate.swprefab", "engine.pose.v1"}};
    s.spawnPrefab = makeCrate;
    return s;
}

size_t count(World& world, const char* name) {
    size_t n = 0;
    world.each<const Name>([&](Entity, const Name& v) { n += v.value == name; });
    return n;
}

size_t replicas(World& world) {
    size_t n = 0;
    world.each<const NetworkIdentity>([&](Entity, const NetworkIdentity&) { ++n; });
    return n;
}

// ---- Real host and clients --------------------------------------------------------------

struct Machine {
    World world;
    std::unique_ptr<LoopbackOnlineServices> services;
    std::unique_ptr<NetworkWorld> net;
    Machine(LoopbackHub& hub, uint64_t identity, NetworkStart start, int order) {
        makeScene(world, order);
        services = std::make_unique<LoopbackOnlineServices>(hub, PeerId{identity});
        net = std::make_unique<NetworkWorld>(start, services.get(), &world);
    }
};

struct Session {
    LoopbackHub hub;
    std::vector<std::unique_ptr<Machine>> machines;
    uint32_t tick = 0;
    std::function<void(Machine& host, uint32_t tick)> onHostTick;

    explicit Session(LoopbackHubConfig config = {}) : hub(config) {}
    Machine& add(NetworkRole role, int order, NetworkStart start) {
        start.role = role;
        machines.push_back(std::make_unique<Machine>(hub, 100 + machines.size(), start, order));
        Machine& m = *machines.back();
        if (auto begun = m.net->begin(); !begun) check(false, std::string("begin: ") + begun.error().message);
        return m;
    }
    Machine& add(NetworkRole role, int order) { return add(role, order, startFor(role)); }
    void step() {
        const double now = double(hub.nowUs()) / 1e6;
        for (auto& m : machines) {
            m->net->pump(now);
            m->net->drain(tick);
            if (m->net->role() == NetworkRole::Authority && onHostTick) onHostTick(*m, tick);
            m->net->endTick(tick);
        }
        hub.advance(16667);
        ++tick;
    }
    bool until(const std::function<bool()>& done, int steps) {
        for (int i = 0; i < steps; ++i) {
            if (done()) return true;
            step();
        }
        return done();
    }
    void run(int steps) {
        for (int i = 0; i < steps; ++i) step();
    }
};

// Every client's objects match the host's: IDs, owners, incarnations and poses.
void checkConverged(Machine& host, Machine& client, const std::string& what) {
    check(client.net->objectCount() == host.net->objectCount(), what + ": the client has the host's object count");
    check(replicas(client.world) == host.net->objectCount(), what + ": one replica entity per object");
    host.world.each<const NetworkIdentity, const Transform>([&](Entity, const NetworkIdentity& id, const Transform& t) {
        const Entity e = client.net->entity(id.id);
        const NetworkIdentity* theirs = client.net->identity(e);
        const Transform* tt = client.world.get<Transform>(e);
        if (!theirs || !tt) return check(false, what + ": the client lacks object " + std::to_string(id.id));
        check(
            theirs->ownerSlot == id.ownerSlot && theirs->incarnation == id.incarnation,
            what + ": owner and incarnation of " + std::to_string(id.id));
        check(glm::length(tt->position - t.position) < 1e-5f, what + ": pose of " + std::to_string(id.id));
    });
}

void keysAndOrders() {
    Session s;
    Machine& host = s.add(NetworkRole::Authority, 0);
    Machine& a = s.add(NetworkRole::Client, 1);
    Machine& b = s.add(NetworkRole::Client, 2);
    check(host.net->objectCount() == 3, "the host numbers its three authored objects");
    // IDs follow key order, not entity order.
    check(host.net->networkId(byKey(host.world, "alpha")) == 1, "alpha is network object 1");
    check(host.net->networkId(byKey(host.world, "charlie")) == 3, "charlie is network object 3");
    check(
        s.until([&] { return a.net->state() == NetworkState::Active && b.net->state() == NetworkState::Active; }, 120),
        "both clients become active");
    for (const char* key : {"alpha", "bravo", "charlie"}) {
        const uint64_t id = host.net->networkId(byKey(host.world, key));
        check(a.net->networkId(byKey(a.world, key)) == id, std::string("client A binds ") + key + " by key");
        check(b.net->networkId(byKey(b.world, key)) == id, std::string("client B binds ") + key + " by key");
    }
    check(
        byKey(a.world, "alpha").index != byKey(host.world, "alpha").index,
        "local entity handles really differ between machines");
    // Movement on the host reaches both, a snapshot interval or so later.
    s.onHostTick = [&](Machine& h, uint32_t tick) {
        h.world.get<Transform>(byKey(h.world, "bravo"))->position = {float(tick) * 0.1f, 2, 3};
    };
    s.run(30);
    s.onHostTick = nullptr;
    s.run(10);
    checkConverged(host, a, "keys");
    checkConverged(host, b, "keys");
    check(a.net->stats().recordsApplied > 0, "clients apply snapshot records");
    check(!a.net->hasAuthority(byKey(a.world, "bravo")), "a client has no authority over the host's objects");
    check(host.net->hasAuthority(byKey(host.world, "bravo")), "the host has");
}

void spawnsDespawnsLateJoin() {
    Session s;
    Machine& host = s.add(NetworkRole::Authority, 0);
    // Before anyone joins: two crates, and one authored object despawned.
    Transform at;
    at.position = {1, 2, 3};
    auto first = host.net->spawn(kCrate, at, 0);
    auto second = host.net->spawn(kCrate, at, 1);
    check(first && second, "the host spawns manifest prefabs");
    check(!host.net->spawn(77, at), "a prefab outside the manifest can't be spawned");
    check(!host.net->spawn(kCrate, at, 9), "nor owned by a slot over max_players");
    check(bool(host.net->despawn(byKey(host.world, "charlie"))), "an authored object can be despawned");
    Machine& client = s.add(NetworkRole::Client, 1);
    check(s.until([&] { return client.net->state() == NetworkState::Active; }, 120), "a late client becomes active");
    check(count(client.world, "crate") == 2, "it gets the crates spawned before it joined");
    check(!byKey(client.world, "charlie").valid(), "and destroys the authored object the host despawned first");
    // Now spawns, owners, a respawn and despawns while it's in.
    auto third = host.net->spawn(kCrate, at, 2);
    check(bool(third), "a spawn while the client is active");
    s.run(10);
    const Entity mine = client.net->entity(host.net->networkId(third.value()));
    check(mine.valid() && client.net->isLocallyControlled(mine), "the client owns the crate it was given");
    check(client.net->identity(mine)->ownerSlot == client.net->localPlayer(), "owner slot is the client's");
    const uint32_t before = host.net->identity(first.value())->incarnation;
    Transform away;
    away.position = {0, 50, 0};
    check(bool(host.net->respawn(first.value(), &away)), "a respawn");
    check(host.net->identity(first.value())->incarnation == before + 1, "makes a new incarnation");
    check(bool(host.net->setOwner(second.value(), 0)), "an owner change");
    check(bool(host.net->despawn(second.value())), "a despawn");
    host.world.destroy(third.value());   // destroyed without despawn(): swept at the end of the tick
    s.run(20);
    check(count(client.world, "crate") == 1, "despawned and destroyed crates are gone on the client");
    checkConverged(host, client, "spawns");
    const Entity respawned = client.net->entity(host.net->networkId(first.value()));
    check(
        respawned.valid() && client.world.get<Transform>(respawned)->position.y == 50,
        "the respawn arrived with its new pose");
    check(client.net->stats().respawns == 1, "as one respawn");
    // The client can't change network objects.
    check(!client.net->spawn(kCrate, at), "a client can't spawn");
    check(!client.net->despawn(respawned), "or despawn");
    // A player leaving gives up its objects.
    auto owned = host.net->spawn(kCrate, at, client.net->localPlayer());
    s.run(5);
    client.net->close();
    s.run(5);
    check(host.net->identity(owned.value())->ownerSlot == 0, "a player's objects have no owner once it leaves");
}

// A small outbound budget: the host sends its players' objects every snapshot and the
// rest in turn, and never more than the budget allows.
void budget() {
    Session s;
    NetworkStart hostStart = startFor(NetworkRole::Authority);
    // Lifecycle messages keep 16 KiB/s, or half of a smaller budget: 12 KiB/s for
    // snapshots, about 600 bytes each at 20 a second.
    hostStart.settings.outboundBytesPerSecond = 24576;
    Machine& host = s.add(NetworkRole::Authority, 0, hostStart);
    Transform at;
    for (int i = 0; i < 40; ++i) (void)host.net->spawn(kCrate, at, 0);
    Machine& client = s.add(NetworkRole::Client, 1);
    check(s.until([&] { return client.net->state() == NetworkState::Active; }, 240), "a client joins a busy host");
    const auto mine = host.net->spawn(kCrate, at, client.net->localPlayer());
    s.run(20);
    const uint64_t sentBefore = host.net->stats().snapshotBytesSent;
    s.onHostTick = [&](Machine& h, uint32_t tick) {
        h.world.each<NetworkIdentity, Transform>(
            [&](Entity, NetworkIdentity&, Transform& t) { t.position.x = float(tick) * 0.01f; });
    };
    const uint64_t appliedBefore = client.net->stats().recordsApplied;
    s.run(60);   // warm up: queues and scratch reach their size
    const uint64_t allocations = g_allocations;
    s.run(540);   // ten seconds in all
    const uint64_t allocated = g_allocations - allocations;
    check(allocated == 0, "no heap allocation while replicating (" + std::to_string(allocated) + ")");
    const double perSecond = double(host.net->stats().snapshotBytesSent - sentBefore) / 10.0;
    check(
        perSecond <= 12288 + 1024,
        "snapshots stay within the budget minus the control reserve (" + std::to_string(int(perSecond)) + " B/s)");
    check(host.net->stats().recordsLimited > 0, "the budget held records back");
    check(client.net->stats().recordsApplied > appliedBefore, "the client still gets updates");
    // The owned crate goes in every snapshot; the rest take turns, so all converge.
    s.onHostTick = nullptr;
    s.run(300);
    checkConverged(host, client, "budget");
    (void)mine;
}

// The host goes away halfway through a slow baseline: the client fails, and its world is
// exactly as it was (nothing spawned, bound or changed).
void baselineTeardown() {
    Session s;
    NetworkStart hostStart = startFor(NetworkRole::Authority);
    hostStart.settings.outboundBytesPerSecond = 4096;   // the baseline takes seconds
    hostStart.settings.maxReplicatedEntities = 512;
    Machine& host = s.add(NetworkRole::Authority, 0, hostStart);
    Transform at;
    for (int i = 0; i < 300; ++i) (void)host.net->spawn(kCrate, at, 0);
    Machine& client = s.add(NetworkRole::Client, 1);
    const World before = client.world;
    check(s.until([&] { return client.net->state() == NetworkState::Synchronizing; }, 120), "the client synchronizes");
    s.run(30);
    check(client.net->state() == NetworkState::Synchronizing, "the baseline is still arriving");
    host.net->close();
    check(s.until([&] { return client.net->state() == NetworkState::Failed; }, 60), "the client fails");
    check(client.net->failure() == ReasonCode::HostLeft, "with HostLeft");
    check(client.net->objectCount() == 0, "its staged objects are gone");
    check(count(client.world, "crate") == 0 && replicas(client.world) == 0, "nothing was spawned or bound");
    check(client.world.entityCount() == before.entityCount(), "the world has the same entities");
    check(byKey(client.world, "alpha").valid(), "authored objects are untouched");
}

// ---- A scripted host ---------------------------------------------------------------------

// Plays the host's side by hand: answers Hello with Challenge and Welcome, then sends
// whatever the test says, encoded with the real codec.
struct ScriptedHost final : TransportSink {
    LoopbackHub& hub;
    std::unique_ptr<ITransport> transport;
    ConnectionId connection;
    Nonce session{};
    std::vector<MessageKind> received;
    uint32_t readyCount = 0, resyncCount = 0;
    bool closed = false;
    ReasonCode closedReason = ReasonCode::Normal;
    uint16_t maxEntities = 512;

    explicit ScriptedHost(LoopbackHub& h) : hub(h), transport(h.createTransport(PeerId{1})) {
        for (size_t i = 0; i < session.size(); ++i) session[i] = uint8_t(0xa0 + i);
        TransportLimits limits;
        (void)transport->listen(kAddress, limits);
    }
    void onEvent(const TransportEvent& event) override {
        if (event.kind == TransportEventKind::Incoming) {
            (void)transport->accept(event.connection);
            connection = event.connection;
        }
        if (event.kind == TransportEventKind::Closed) {
            closed = true;
            closedReason = event.reason;
        }
    }
    void onMessage(const ReceivedMessage& message) override {
        DecodeOptions options;
        options.delivery = message.delivery;
        DecodedMessage decoded;
        if (!decodeMessage(message.bytes, options, decoded)) return;
        received.push_back(kindOf(decoded.body));
        if (const auto* hello = std::get_if<Hello>(&decoded.body)) {
            Challenge c;
            c.clientNonce = hello->clientNonce;
            c.gameProtocol = "replication-test";
            c.schemaDigest = registry().schemaDigest();
            c.contentDigest = {};
            c.sceneId = 1;
            c.hostPeer = transport->localPeer().value;
            send(c);
            Welcome welcome;
            welcome.playerSlot = 2;
            welcome.maxEntities = maxEntities;
            send(welcome);
        }
        readyCount += std::holds_alternative<Ready>(decoded.body);
        resyncCount += std::holds_alternative<ResyncRequest>(decoded.body);
    }
    void send(const MessageBody& body, uint32_t tick = 0) {
        std::array<uint8_t, kMaxReliableMessageBytes> out{};
        uint32_t written = 0;
        if (!encodeMessage(body, session, tick, out, written)) return check(false, "the test's message encodes");
        const MessageDescriptor* d = findMessage(uint8_t(kindOf(body)));
        (void)transport->send(connection, std::span(out.data(), written), d->delivery);
    }
    void poll() { transport->poll(*this, {}); }
};

std::vector<uint8_t> poseBytes(glm::vec3 position, glm::quat rotation = {1, 0, 0, 0}) {
    const ReplicationProfile& profile = *registry().profile(kProfilePose);
    ProfileValue value;
    setVec3(value.fields[0], position.x, position.y, position.z);
    setQuat(value.fields[1], rotation.x, rotation.y, rotation.z, rotation.w);
    std::vector<uint8_t> bytes(profile.encodedBytes);
    uint32_t written = 0;
    (void)encodeProfile(profile, value, bytes, written);
    return bytes;
}

// One baseline entry, as the scripted host describes it.
struct Entry {
    uint64_t id = 0;
    uint32_t incarnation = 0;
    uint16_t owner = 0;
    uint16_t prefab = 0;
    std::string key;
    std::vector<uint8_t> state;
};

// The baseline digest, written here from docs/Networking.md rather than taken from the code.
Digest digestOf(const std::vector<Entry>& entries) {
    Sha256 hash;
    hash.update(std::string_view("shockwave_baseline 1\n"));
    hash.updateU32(uint32_t(entries.size()));
    for (const Entry& e : entries) {
        hash.updateU64(e.id);
        hash.updateU32(e.incarnation);
        hash.updateU16(e.owner);
        hash.updateU16(e.prefab);
        hash.updateU16(uint16_t(e.key.size()));
        hash.update(std::string_view(e.key));
        hash.updateU16(kProfilePose);
        hash.updateU16(uint16_t(e.state.size()));
        hash.update(std::span<const uint8_t>(e.state));
    }
    return hash.finish();
}

struct Scripted {
    LoopbackHub hub;
    ScriptedHost host{hub};
    World world;
    std::unique_ptr<LoopbackOnlineServices> services;
    std::unique_ptr<NetworkWorld> client;
    uint32_t serial = 0;

    explicit Scripted(int order = 1) {
        makeScene(world, order);
        services = std::make_unique<LoopbackOnlineServices>(hub, PeerId{2});
        client = std::make_unique<NetworkWorld>(startFor(NetworkRole::Client), services.get(), &world);
        if (!client->begin()) check(false, "the scripted client starts");
        until([&] { return client->state() == NetworkState::Synchronizing; }, 30);
    }
    void step() {
        client->pump(double(hub.nowUs()) / 1e6);
        host.poll();
        hub.advance(16667);
    }
    bool until(const std::function<bool()>& done, int steps) {
        for (int i = 0; i < steps; ++i) {
            if (done()) return true;
            step();
        }
        return done();
    }
    Spawn spawnOf(const Entry& e, uint32_t controlSerial) {
        Spawn m;
        m.controlSerial = controlSerial;
        m.entity = e.id;
        m.incarnation = e.incarnation;
        m.ownerSlot = e.owner;
        m.prefabId = e.prefab;
        m.authoredKey = e.key;
        m.profileId = kProfilePose;
        m.initialState = e.state;
        return m;
    }
    // A baseline of the three authored objects and one crate; the client becomes Active.
    std::vector<Entry> standard() {
        return {
            {1, 0, 0, 0, "alpha", poseBytes({1, 0, 0})},
            {2, 0, 0, 0, "bravo", poseBytes({2, 0, 0})},
            {3, 0, 0, 0, "charlie", poseBytes({3, 0, 0})},
            {4, 0, 2, kCrate, "", poseBytes({4, 0, 0})},
        };
    }
    void baseline(const std::vector<Entry>& entries, bool activate = true, std::optional<Digest> digest = {}) {
        BaselineBegin begin;
        begin.objectCount = uint16_t(entries.size());
        host.send(begin);
        for (const Entry& e : entries) host.send(spawnOf(e, ++serial));
        BaselineEnd end;
        end.finalControlSerial = serial;
        end.baselineDigest = digest ? *digest : digestOf(entries);
        host.send(end);
        if (!activate) return;
        until([&] { return host.readyCount > 0 || client->state() == NetworkState::Failed; }, 30);
        Active active;
        active.controlledEntity = 4;
        host.send(active);
        until([&] { return client->state() != NetworkState::Synchronizing; }, 30);
    }
    void snapshot(
        uint32_t tick, uint32_t required, uint16_t part, uint16_t parts,
        std::vector<std::pair<uint64_t, glm::vec3>> records, uint32_t incarnation = 0) {
        std::array<uint8_t, kMaxUnreliableMessageBytes> buffer{};
        WireWriter writer(buffer);
        std::vector<std::vector<uint8_t>> states;
        for (const auto& [id, position] : records) {
            states.push_back(poseBytes(position));
            StateRecord r;
            r.entity = id;
            r.incarnation = incarnation;
            r.profileId = kProfilePose;
            r.state = states.back();
            (void)writeStateRecord(writer, r);
        }
        Snapshot m;
        m.snapshotId = tick;
        m.serverTick = tick;
        m.requiredControlSerial = required;
        m.partIndex = part;
        m.partCount = parts;
        m.recordCount = uint16_t(records.size());
        m.records = writer.written();
        host.send(m, tick);
    }
    glm::vec3 position(const char* key) { return world.get<Transform>(byKey(world, key))->position; }
    glm::vec3 positionOf(uint64_t id) {
        const Entity e = client->entity(id);
        return e.valid() ? world.get<Transform>(e)->position : glm::vec3(-999);
    }
};

void scriptedOrdering() {
    Scripted t;
    t.baseline(t.standard());
    check(t.client->state() == NetworkState::Active, "a scripted baseline makes the client active");
    check(t.host.readyCount == 1, "the client sent Ready once");
    check(t.position("alpha") == glm::vec3(1, 0, 0), "authored state is applied at commit");
    check(count(t.world, "crate") == 1 && t.positionOf(4) == glm::vec3(4, 0, 0), "the crate is spawned and placed");
    check(t.client->controlledEntity() == t.client->entity(4), "Active names the controlled crate");

    // Parts in reverse order: each applies on its own; the older tick's part doesn't undo
    // the newer one's.
    t.snapshot(10, t.serial, 1, 2, {{2, {20, 0, 0}}});
    t.snapshot(10, t.serial, 0, 2, {{1, {10, 0, 0}}});
    t.step();
    check(
        t.position("alpha") == glm::vec3(10, 0, 0) && t.position("bravo") == glm::vec3(20, 0, 0),
        "parts apply in any order");
    t.snapshot(10, t.serial, 0, 2, {{1, {11, 0, 0}}});   // a duplicate part
    t.snapshot(8, t.serial, 0, 1, {{1, {8, 0, 0}}, {2, {8, 0, 0}}});   // an older snapshot
    t.step();
    check(t.position("alpha") == glm::vec3(10, 0, 0), "a duplicate or older record changes nothing");
    check(t.client->stats().staleRecords == 3, "and counts as stale");

    // Despawn, then an old snapshot naming the object: it stays gone.
    Despawn despawn;
    despawn.controlSerial = ++t.serial;
    despawn.entity = 4;
    t.host.send(despawn);
    t.step();
    check(count(t.world, "crate") == 0, "the crate is despawned");
    t.snapshot(9, t.serial - 1, 0, 1, {{4, {9, 9, 9}}});
    t.step();
    check(
        count(t.world, "crate") == 0 && t.client->state() == NetworkState::Active,
        "an old snapshot doesn't bring it back");

    // A snapshot for a crate whose Spawn hasn't arrived waits for it, then applies.
    t.snapshot(20, t.serial + 1, 0, 1, {{5, {5, 5, 5}}});
    t.step();
    check(
        t.client->stats().heldSnapshots == 1 && t.client->entity(5) == kNullEntity,
        "a snapshot ahead of its Spawn is held");
    Entry crate{5, 0, 0, kCrate, "", poseBytes({0, 0, 0})};
    t.host.send(t.spawnOf(crate, ++t.serial));
    t.step();
    check(t.positionOf(5) == glm::vec3(5, 5, 5), "and applies once the Spawn arrives");

    // One that never gets its control serial expires after 250 ms and asks for a resync.
    t.snapshot(30, t.serial + 5, 0, 1, {{1, {30, 0, 0}}});
    for (int i = 0; i < 30; ++i) t.step();
    check(t.client->stats().expiredSnapshots == 1, "a held snapshot expires");
    check(t.host.resyncCount == 1, "and the client asks for a resync, once");
    check(t.client->state() == NetworkState::Active, "the client stays in the game");

    // A respawn: snapshots of the old incarnation are stale, the new one applies.
    Entry respawned = crate;
    respawned.incarnation = 1;
    respawned.state = poseBytes({0, 40, 0});
    t.host.send(t.spawnOf(respawned, ++t.serial));
    t.snapshot(40, t.serial, 0, 1, {{5, {1, 1, 1}}}, 0);
    t.step();
    check(t.positionOf(5) == glm::vec3(0, 40, 0), "a record from the old incarnation is ignored");
    t.snapshot(41, t.serial, 0, 1, {{5, {2, 41, 0}}}, 1);
    t.step();
    check(t.positionOf(5) == glm::vec3(2, 41, 0), "the new incarnation's record applies");
}

struct BadCase {
    const char* name;
    ReasonCode expected;
    std::function<void(Scripted&)> act;
    bool beforeCommit = true;   // the world must be untouched
};

void malformedAuthority() {
    const BadCase cases[] = {
        {"a prefab outside the manifest", ReasonCode::MalformedPacket,
         [](Scripted& t) {
             auto e = t.standard();
             e[3].prefab = 9;
             t.baseline(e, false);
         }},
        {"an authored key the scene lacks", ReasonCode::ContentMismatch,
         [](Scripted& t) {
             auto e = t.standard();
             e[0].key = "delta";
             t.baseline(e, false);
         }},
        {"a wrong baseline digest", ReasonCode::MalformedPacket,
         [](Scripted& t) {
             Digest wrong{};
             wrong[0] = 1;
             t.baseline(t.standard(), false, wrong);
         }},
        {"one object twice", ReasonCode::MalformedPacket,
         [](Scripted& t) {
             auto e = t.standard();
             e[1].id = 1;
             t.baseline(e, false);
         }},
        {"a quaternion that isn't unit length", ReasonCode::MalformedPacket,
         [](Scripted& t) {
             auto e = t.standard();
             e[2].state = poseBytes({0, 0, 0}, glm::quat(2, 0, 0, 0));
             t.baseline(e, false);
         }},
        {"an owner over max_players", ReasonCode::MalformedPacket,
         [](Scripted& t) {
             auto e = t.standard();
             e[3].owner = 60;
             t.baseline(e, false);
         }},
        {"more objects than max_entities", ReasonCode::MalformedPacket,
         [](Scripted& t) {
             BaselineBegin begin;
             begin.objectCount = 600;
             t.host.send(begin);
         }},
        {"a control serial gap", ReasonCode::MalformedPacket,
         [](Scripted& t) {
             t.baseline(t.standard());
             Entry crate{5, 0, 0, kCrate, "", poseBytes({0, 0, 0})};
             t.host.send(t.spawnOf(crate, t.serial + 2));
         },
         false},
        {"a snapshot of an object never spawned", ReasonCode::MalformedPacket,
         [](Scripted& t) {
             t.baseline(t.standard());
             t.snapshot(5, t.serial, 0, 1, {{99, {0, 0, 0}}});
         },
         false},
        {"a despawned ID spawned again", ReasonCode::MalformedPacket,
         [](Scripted& t) {
             t.baseline(t.standard());
             Despawn d;
             d.controlSerial = ++t.serial;
             d.entity = 4;
             t.host.send(d);
             Entry crate{4, 1, 0, kCrate, "", poseBytes({0, 0, 0})};
             t.host.send(t.spawnOf(crate, ++t.serial));
         },
         false},
        {"Active naming another player's object", ReasonCode::MalformedPacket,
         [](Scripted& t) {
             auto e = t.standard();
             e[3].owner = 3;
             t.baseline(e);
         },
         false},
    };
    for (const BadCase& c : cases) {
        Scripted t;
        const size_t entities = t.world.entityCount();
        c.act(t);
        t.until([&] { return t.client->state() == NetworkState::Failed; }, 30);
        check(t.client->state() == NetworkState::Failed, std::string(c.name) + ": the client fails");
        check(
            t.client->failure() == c.expected,
            std::string(c.name) + ": with " + reasonName(c.expected) + ", not " +
                (reasonName(t.client->failure()) ? reasonName(t.client->failure()) : "?"));
        check(
            !t.client->hasTransport() && t.client->objectCount() == 0,
            std::string(c.name) + ": everything is released");
        if (c.beforeCommit)
            check(
                t.world.entityCount() == entities && replicas(t.world) == 0 && count(t.world, "crate") == 0,
                std::string(c.name) + ": the world is untouched");
        t.step();
        check(
            t.host.closed && t.host.closedReason == c.expected,
            std::string(c.name) + ": the client closes the connection, giving the reason");
    }
}

// A client never trusts its own messages to the host: the host refuses replication
// messages that only it may send.
void hostRefusesAuthorityMessages() {
    Session s;
    Machine& host = s.add(NetworkRole::Authority, 0);
    Machine& client = s.add(NetworkRole::Client, 1);
    s.until([&] { return client.net->state() == NetworkState::Active; }, 120);
    check(host.net->stats().activePeers == 1, "the client is active on the host");
    check(!client.net->setOwner(client.net->entity(1), 2), "a client can't take ownership");
    check(!client.net->respawn(client.net->entity(1)), "or respawn objects");
}

}   // namespace

int main() {
    keysAndOrders();
    spawnsDespawnsLateJoin();
    budget();
    baselineTeardown();
    scriptedOrdering();
    malformedAuthority();
    hostRefusesAuthorityMessages();
    if (failures) {
        std::fprintf(stderr, "network_replication: %d of %d check(s) failed\n", failures, checks);
        return 1;
    }
    std::printf("network_replication: %d checks passed\n", checks);
    return 0;
}
