// Combat history, game events and network circuits (plan Batch 8, docs/Networking.md
// "Combat and events"): real NetworkWorlds over the loopback hub, with Jolt for the static
// world on the host. Shots are tested where the shooter saw the targets and only within
// what its measured round trip allows: a target behind a wall, view times too old, in
// the future or forged to manufacture a hit, a respawn in between, a delayed real hit at
// low and high latency. Game events arrive once each, in order, never replayed to a late
// joiner, and a scripted host's duplicates are dropped; PresentationDedup plays each shot
// once. Circuits: scope rules at compile time, and at run time an unauthorised circuit
// can't change the game and a duplicated event runs its handler once.
#include <Assets/Assets.h>
#include <Circuits/Circuit.h>
#include <Circuits/CircuitNodes.h>
#include <Circuits/CircuitProgram.h>
#include <Circuits/CircuitWorld.h>
#include <Core/Jobs.h>
#include <Engine/Components.h>
#include <Engine/PhysicsComponents.h>
#include <Engine/Prefab.h>
#include <Multiplayer/NetworkComponents.h>
#include <Multiplayer/NetworkWorld.h>
#include <Multiplayer/PresentationDedup.h>
#include <Multiplayer/Replication.h>
#include <Network/Loopback.h>
#include <Network/LoopbackServices.h>
#include <Network/Messages.h>
#include <Network/Sha256.h>
#include <Network/Wire.h>
#include <Physics/Physics.h>

#include <glm/geometric.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace shockwave;
using namespace shockwave::net;
namespace fs = std::filesystem;

namespace {

int failures = 0, checks = 0;
void check(bool value, const std::string& message) {
    ++checks;
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    }
}

constexpr uint16_t kPawn = 1;
constexpr uint16_t kBoom = 40;   // test.boom {power u16 0..1000}
constexpr uint16_t kAsk = 40;   // test.ask {level u8 0..3}
constexpr TransportAddress kAddress{TransportAddress::Loopback, 1, 0};
constexpr const char* kProtocol = "combat-test";

// A stand-in for a game's health, for the circuits.
struct Health {
    float current = 100;
};

const ReplicationRegistry& registry() {
    static const ReplicationRegistry frozen = [] {
        ReplicationRegistry r;
        registerEngineReplication(r);
        std::string why;
        EventDesc boom;
        boom.id = kBoom;
        boom.name = "test.boom";
        boom.payload = {netInt("power", NetFieldType::U16, 0, 1000, NetFieldRole::Counter)};
        if (!r.addEvent(std::move(boom), why)) check(false, "add test.boom: " + why);
        CommandDesc ask;
        ask.id = kAsk;
        ask.name = "test.ask";
        ask.payload = {netInt("level", NetFieldType::U8, 0, 3, NetFieldRole::State)};
        if (!r.addCommand(std::move(ask), why)) check(false, "add test.ask: " + why);
        std::vector<std::string> problems;
        if (!r.freeze(problems)) check(false, "the test registry freezes: " + (problems.empty() ? "" : problems[0]));
        return r;
    }();
    return frozen;
}

Entity makePawn(World& world, const NetworkPrefab& prefab, const Transform& at) {
    const Entity e = world.create();
    world.add<Transform>(e, at);
    world.add<Name>(e, {"pawn"});
    world.add<CharacterController>(e, {});
    NetworkObject n;
    n.profile = prefab.profile;
    world.add<NetworkObject>(e, n);
    return e;
}

NetworkStart startFor(NetworkRole role) {
    NetworkStart s;
    s.role = role;
    s.settings.tickRate = 60;
    s.settings.snapshotRate = 60;
    s.settings.maxPlayers = 8;
    // Short, so the shooter's round trip is what limits a view time within the history's
    // 250 ms: a client shows remote players 3 ticks behind, with 2 + 1 ticks' tolerance.
    s.settings.interpolationMs = 50;
    s.settings.maxRewindMs = 250;
    s.compatibility.gameProtocol = kProtocol;
    s.compatibility.schemaDigest = registry().schemaDigest();
    s.address = kAddress;
    s.authenticate = false;
    s.registry = &registry();
    s.prefabs = {{kPawn, "Prefabs/Pawn.swprefab", "engine.pose.v1"}};
    s.spawnPrefab = makePawn;
    return s;
}

struct Machine {
    World world;
    std::unique_ptr<LoopbackOnlineServices> services;
    std::unique_ptr<NetworkWorld> net;
    Machine(LoopbackHub& hub, uint64_t identity, NetworkStart start) {
        services = std::make_unique<LoopbackOnlineServices>(hub, PeerId{identity});
        net = std::make_unique<NetworkWorld>(start, services.get(), &world);
    }
};

struct Session {
    LoopbackHub hub;
    std::vector<std::unique_ptr<Machine>> machines;
    uint32_t tick = 1;
    std::function<void(Machine& host, uint32_t tick)> onHostTick;

    explicit Session(LoopbackHubConfig config = {}) : hub(config) {}
    Machine& add(NetworkRole role) {
        machines.push_back(std::make_unique<Machine>(hub, 100 + machines.size(), startFor(role)));
        Machine& m = *machines.back();
        if (auto begun = m.net->begin(); !begun) check(false, std::string("begin: ") + begun.error().message);
        return m;
    }
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

// ---- Lag compensation ----------------------------------------------------------------------

// A host with physics (a floor and a wall), two clients, and a pawn for each of the three
// players. The target (slot 3) walks along x at a quarter metre a tick.
struct Arena {
    Session session;
    Machine& host;
    Machine& a;
    Machine& b;
    JobSystem jobs{1};
    std::unique_ptr<PhysicsWorld> physics;
    Entity mine, shooter, target, wall;
    std::map<uint32_t, glm::vec3> path;   // the target's feet at each host tick
    bool walking = true, targetAlive = true;
    float x = 0;

    static LoopbackHubConfig hubWith(uint32_t oneWayUs) {
        LoopbackHubConfig c;
        c.faults.latencyUs = oneWayUs;
        return c;
    }
    explicit Arena(uint32_t oneWayUs)
        : session(hubWith(oneWayUs)), host(session.add(NetworkRole::Authority)), a(session.add(NetworkRole::Client)),
          b(session.add(NetworkRole::Client)) {
        check(
            session.until(
                [&] { return a.net->state() == NetworkState::Active && b.net->state() == NetworkState::Active; }, 300),
            "both clients become active");
        World& w = host.world;
        auto block = [&](glm::vec3 at, glm::vec3 half) {
            const Entity e = w.create();
            w.add<Transform>(e, {at});
            Collider c;
            c.halfExtents = half;
            w.add<Collider>(e, c);
            RigidBody body;
            body.motion = RigidBody::Motion::Static;
            w.add<RigidBody>(e, body);
            return e;
        };
        block({0, -0.5f, 0}, {80, 0.5f, 80});
        wall = block({40, 1, -5}, {2, 2, 0.1f});   // in front of x = 40 only
        Transform at;
        at.position = {-20, 0, 0};
        mine = host.net->spawn(kPawn, at, 1).value();
        at.position = {0, 0, 0};
        shooter = host.net->spawn(kPawn, at, a.net->localPlayer()).value();
        at.position = {0, 0, -10};
        target = host.net->spawn(kPawn, at, b.net->localPlayer()).value();
        physics = std::make_unique<PhysicsWorld>(w, jobs);
        host.net->setPhysics(physics.get());   // the host's mode: bodies are built for it next
        physics->synchronize();
        host.net->setCombatTargets([this](const World& world, Entity e, CombatTarget& out) {
            const auto* id = world.get<NetworkIdentity>(e);
            if (!id) return false;
            out.team = uint8_t(id->ownerSlot == 3 ? 1 : 0);
            out.alive = e != target || targetAlive;
            out.eyeHeight = 1.5f;
            return true;
        });
        session.onHostTick = [this](Machine& h, uint32_t tick) {
            auto& t = *h.world.get<Transform>(target);
            if (walking) x += 0.25f;
            t.position = {x, 0, -10};
            path[tick] = t.position;
        };
        // Long enough for the host's pings to measure both round trips; then the target
        // starts again near the middle, so misses are clear of it.
        session.run(150);
        x = -2;
        session.run(20);
    }
    uint32_t now() const { return session.tick - 1; }   // the last tick recorded
    // A command from player A that claims its view showed tick `view`.
    InputCommand command(uint32_t view, uint16_t fraction = 0) const {
        InputCommand c;
        c.targetTick = session.tick;
        c.viewTick = view;
        c.viewFraction = fraction;
        c.incarnation = host.net->identity(shooter)->incarnation;
        return c;
    }
    // A shot from origin at the target's chest where it stood at tick `at`.
    ShotResult shoot(Entity from, glm::vec3 origin, const glm::vec3& feet, const InputCommand* c) {
        ShotQuery q;
        q.shooter = from;
        q.origin = origin;
        q.direction = glm::normalize(feet + glm::vec3(0, 0.85f, 0) - origin);
        q.range = 100;
        q.command = c;
        ShotResult r;
        check(host.net->resolveShot(q, r), "the host resolves the shot");
        return r;
    }
    ShotResult shootFromA(uint32_t view, uint32_t aimTick) {
        const InputCommand c = command(view);
        return shoot(shooter, {0, 1.5f, 0}, path.at(aimTick), &c);
    }
};

void rewindAtLatency(uint32_t oneWayUs, const char* label) {
    Arena t(oneWayUs);
    const std::string at = std::string(" (") + label + ")";
    const uint16_t slotA = t.a.net->localPlayer();
    check(t.host.net->roundTrip(slotA) > 0, "the host measured A's round trip" + at);
    check(t.host.net->stats().rttMs > 0, "and shows the worst in its stats" + at);
    const uint32_t rttTicks = uint32_t(std::ceil(t.host.net->roundTrip(slotA) * 60));
    std::printf("Combat %s: A's round trip %.0f ms (%u ticks)\n", label, t.host.net->roundTrip(slotA) * 1000, rttTicks);

    // A delayed real hit: the client saw the target 5 ticks ago and aimed there.
    const uint32_t now = t.now();
    ShotResult r = t.shootFromA(now - 5, now - 5);
    check(r.player && r.hit == t.target && r.rewind == ShotRewind::Exact, "a delayed real hit counts" + at);
    check(r.viewTick == now - 5, "tested at the tick the command claimed" + at);
    check(r.hitIncarnation == t.host.net->identity(t.target)->incarnation, "in the target's current life" + at);
    // Aimed where the target is now with the same claimed view: it wasn't there then.
    r = t.shootFromA(now - 5, now);
    check(!r.player, "the target's present position isn't what the client saw" + at);
    // A view time in the future is moved back to the newest a client can show.
    r = t.shootFromA(now + 6, now - 1);
    check(r.rewind == ShotRewind::Clamped, "a view time in the future is clamped" + at);
    // No view time at all can't be lag compensated.
    r = t.shootFromA(0, now - 5);
    check(r.rewind == ShotRewind::Refused && !r.player, "a command without a view time is refused" + at);
    // Forged: a view time older than A's round trip allows, aimed where the target was
    // then, would make a hit the client never saw.
    const uint32_t forged = now - (rttTicks + 3 + 3 + 3);
    r = t.shootFromA(forged, forged);
    check(r.rewind == ShotRewind::Refused && !r.player, "a forged old view time manufactures no hit" + at);
    // Out of the history window altogether.
    r = t.shootFromA(now - 200, now - 5);
    check(r.rewind == ShotRewind::Refused && !r.player, "a view time out of the window is refused" + at);
    // The host's own player sees the present.
    const InputCommand c = t.command(now - 5);
    r = t.shoot(t.mine, {-20, 1.5f, 0}, t.path.at(now), &c);
    check(r.rewind == ShotRewind::Present && r.player && r.hit == t.target, "the host's own shot is the present" + at);

    // A target behind a static wall: the world ray stops first.
    t.walking = false;
    t.x = 40;
    t.session.run(10);
    const uint32_t still = t.now();
    const InputCommand behind = t.command(still - 5);
    r = t.shoot(t.shooter, {40, 1.5f, 0}, t.path.at(still - 5), &behind);
    check(!r.player && r.hit == t.wall, "a target behind a static wall can't be hit" + at);
    check(r.distance < 5.5f, "the shot stops at the wall" + at);
    // Without the wall in the way, the same rewind hits.
    t.x = 30;
    t.session.run(10);
    const uint32_t open = t.now();
    r = t.shootFromA(open - 5, open - 5);
    check(r.player && r.hit == t.target, "the same shot with a clear line hits" + at);

    // A dead target isn't there.
    t.targetAlive = false;
    t.session.run(10);
    r = t.shootFromA(t.now() - 5, t.now() - 5);
    check(!r.player, "a dead player can't be hit" + at);
    t.targetAlive = true;
    t.session.run(10);

    // A respawn between the view time and now: the hit is in the old life.
    const uint32_t before = t.now() - 4;
    const uint32_t oldLife = t.host.net->identity(t.target)->incarnation;
    check(bool(t.host.net->respawn(t.target)), "the target respawns");
    t.session.run(1);
    r = t.shootFromA(before, before);
    check(
        r.player && r.hitIncarnation == oldLife && t.host.net->identity(t.target)->incarnation == oldLife + 1,
        "a hit from before a respawn names the old life, so the game can refuse it" + at);
    t.session.run(10);
    r = t.shootFromA(t.now() - 5, t.now() - 5);
    check(r.player && r.hitIncarnation == oldLife + 1, "after the respawn, hits are in the new life" + at);

    // Clients can't resolve shots.
    ShotResult none;
    check(!t.a.net->resolveShot(ShotQuery{}, none), "a client can't resolve a shot" + at);
    const auto& s = t.host.net->stats();
    check(s.shotsRewound > 0 && s.shotsClamped > 0 && s.shotsRefused >= 3, "the stats count each kind" + at);
}

void rewind() {
    rewindAtLatency(10'000, "20 ms");
    rewindAtLatency(60'000, "120 ms");
    // A view time that's plausible at 120 ms but not at 20 ms.
    Arena slow(60'000), fast(10'000);
    const uint32_t back = 12;
    ShotResult r = slow.shootFromA(slow.now() - back, slow.now() - back);
    check(r.player && r.rewind == ShotRewind::Exact, "12 ticks back is a real hit at 120 ms");
    r = fast.shootFromA(fast.now() - back, fast.now() - back);
    check(!r.player && r.rewind == ShotRewind::Refused, "but too old at 20 ms");
}

// ---- Game events ---------------------------------------------------------------------------

ProfileValue power(int value) {
    ProfileValue v;
    v.fields[0].u = uint64_t(value);
    return v;
}

void events() {
    LoopbackHubConfig config;
    config.faults.latencyUs = 30'000;
    config.faults.jitterUs = 10'000;
    config.faults.reliableLoss = 0.2;
    config.faults.unreliableLoss = 0.1;
    config.faults.duplication = 0.2;
    Session s(config);
    Machine& host = s.add(NetworkRole::Authority);
    Machine& a = s.add(NetworkRole::Client);
    check(s.until([&] { return a.net->state() == NetworkState::Active; }, 600), "the client becomes active");
    uint64_t hostCursor = host.net->gameEventCursor(), clientCursor = a.net->gameEventCursor();
    for (int i = 1; i <= 20; ++i) {
        check(bool(host.net->sendEvent(kBoom, kNullEntity, power(i * 10))), "the host sends an event");
        s.step();
    }
    check(!host.net->sendEvent(99, kNullEntity, {}), "an unregistered event is refused");
    check(!host.net->sendEvent(kBoom, kNullEntity, power(5000)), "so is a payload out of range");
    check(!a.net->sendEvent(kBoom, kNullEntity, power(1)), "and a client can't send one");
    s.run(120);
    std::vector<uint64_t> seen;
    ReceivedGameEvent e;
    while (a.net->readGameEvent(clientCursor, e)) {
        check(e.eventId == kBoom && e.event && e.event->name == "test.boom", "the client knows the event");
        check(e.payload.fields[0].u == uint64_t(seen.size() + 1) * 10, "its payload arrives as sent");
        seen.push_back(e.sequence);
    }
    check(seen.size() == 20, "all twenty events arrive once each (" + std::to_string(seen.size()) + ")");
    bool ordered = true;
    for (size_t i = 0; i < seen.size(); ++i) ordered = ordered && seen[i] == i + 1;
    check(ordered, "in order, numbered from 1");
    size_t hostSide = 0;
    while (host.net->readGameEvent(hostCursor, e)) ++hostSide;
    check(hostSide == 20, "the host reads its own events too");
    check(a.net->stats().gameEventsReceived == 20, "the client counted twenty");

    // A late joiner gets the durable state, not old events.
    Machine& late = s.add(NetworkRole::Client);
    check(s.until([&] { return late.net->state() == NetworkState::Active; }, 600), "a late client becomes active");
    uint64_t lateCursor = 0;
    size_t old = 0;
    while (late.net->readGameEvent(lateCursor, e)) ++old;
    check(old == 0, "a late joiner sees none of the old events");
    check(bool(host.net->sendEvent(kBoom, kNullEntity, power(7))), "one more event");
    s.run(120);
    lateCursor = 0;
    check(late.net->readGameEvent(lateCursor, e) && e.sequence == 21, "the late joiner gets the new one");
    check(!late.net->readGameEvent(lateCursor, e), "only that one");

    // A reader that falls behind skips to what's kept.
    for (int i = 0; i < int(NetworkWorld::kGameEventHistory) + 10; ++i)
        (void)host.net->sendEvent(kBoom, kNullEntity, power(1));
    uint64_t behind = 0;
    size_t read = 0;
    while (host.net->readGameEvent(behind, e)) ++read;
    check(read == NetworkWorld::kGameEventHistory, "a reader behind gets the newest kGameEventHistory");
    check(host.net->stats().gameEventsMissed > 0, "and the skip is counted");
}

// A host that says exactly what the test wants: a client made Active, then events,
// some of them twice.
struct ScriptedHost final : TransportSink {
    std::unique_ptr<ITransport> transport;
    ConnectionId connection;
    Nonce session{};
    uint32_t readyCount = 0;

    explicit ScriptedHost(LoopbackHub& h) : transport(h.createTransport(PeerId{1})) {
        for (size_t i = 0; i < session.size(); ++i) session[i] = uint8_t(0xb0 + i);
        (void)transport->listen(kAddress, TransportLimits{});
    }
    void onEvent(const TransportEvent& event) override {
        if (event.kind == TransportEventKind::Incoming) {
            (void)transport->accept(event.connection);
            connection = event.connection;
        }
    }
    void onMessage(const ReceivedMessage& message) override {
        DecodeOptions options;
        options.delivery = message.delivery;
        DecodedMessage decoded;
        if (!decodeMessage(message.bytes, options, decoded)) return;
        if (const auto* hello = std::get_if<Hello>(&decoded.body)) {
            Challenge c;
            c.clientNonce = hello->clientNonce;
            c.gameProtocol = kProtocol;
            c.schemaDigest = registry().schemaDigest();
            c.contentDigest = {};
            c.sceneId = 1;
            c.hostPeer = transport->localPeer().value;
            const NetworkSettings settings = startFor(NetworkRole::Authority).settings;
            c.tickRate = uint16_t(settings.tickRate);
            c.snapshotRate = uint16_t(settings.snapshotRate);
            c.interpolationMs = uint16_t(settings.interpolationMs);
            c.maxRewindMs = uint16_t(settings.maxRewindMs);
            send(c);
            Welcome welcome;
            welcome.playerSlot = 2;
            welcome.maxEntities = 64;
            send(welcome);
        }
        readyCount += std::holds_alternative<Ready>(decoded.body);
    }
    void send(const MessageBody& body, uint32_t tick = 0) {
        std::array<uint8_t, kMaxReliableMessageBytes> out{};
        uint32_t written = 0;
        if (!encodeMessage(body, session, tick, out, written)) return check(false, "the test's message encodes");
        (void)transport->send(connection, std::span(out.data(), written), findMessage(uint8_t(kindOf(body)))->delivery);
    }
    void event(uint32_t seq, int value) {
        const EventDesc& desc = *registry().event(kBoom);
        std::array<uint8_t, 64> bytes{};
        uint32_t written = 0;
        if (!encodeFields(desc.payload, power(value), bytes, written)) check(false, "the test's payload encodes");
        GameEvent m;
        m.eventSeq = seq;
        m.eventTick = seq;
        m.eventId = kBoom;
        m.payload = std::span<const uint8_t>(bytes.data(), written);
        send(m);
    }
    void poll() { transport->poll(*this, {}); }
};

struct Scripted {
    LoopbackHub hub;
    ScriptedHost host{hub};
    World world;
    std::unique_ptr<LoopbackOnlineServices> services;
    std::unique_ptr<NetworkWorld> client;

    Scripted() {
        services = std::make_unique<LoopbackOnlineServices>(hub, PeerId{2});
        client = std::make_unique<NetworkWorld>(startFor(NetworkRole::Client), services.get(), &world);
        if (!client->begin()) check(false, "the scripted client starts");
        until([&] { return client->state() == NetworkState::Synchronizing; }, 30);
        host.send(BaselineBegin{});
        BaselineEnd end;
        Sha256 hash;   // the empty baseline's digest (docs/Networking.md)
        hash.update(std::string_view("shockwave_baseline 1\n"));
        hash.updateU32(0);
        end.baselineDigest = hash.finish();
        host.send(end);
        until([&] { return host.readyCount > 0 || client->state() == NetworkState::Failed; }, 30);
        host.send(Active{});
        until([&] { return client->state() != NetworkState::Synchronizing; }, 30);
        check(client->state() == NetworkState::Active, "the scripted client becomes active");
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
};

void duplicateEvents() {
    Scripted t;
    uint64_t cursor = t.client->gameEventCursor();
    t.host.event(1, 10);
    t.host.event(1, 10);   // the same event again
    t.host.event(2, 20);
    t.host.event(1, 10);   // and an old one after a newer
    for (int i = 0; i < 5; ++i) t.step();
    ReceivedGameEvent e;
    std::vector<uint64_t> got;
    while (t.client->readGameEvent(cursor, e)) got.push_back(e.sequence);
    check(got == std::vector<uint64_t>{1, 2}, "duplicated events are read once each");
    check(t.client->stats().duplicateEvents == 2, "and the copies are counted");
    check(t.client->state() == NetworkState::Active, "a duplicate isn't an error");
}

// ---- PresentationDedup ---------------------------------------------------------------------

void dedup() {
    PresentationDedup d;
    using Seen = PresentationDedup::Seen;
    const ShotKey shot{7, 1, 100, 0};
    check(d.predicted(shot), "a predicted shot is shown");
    check(!d.predicted(shot), "once");
    check(d.confirmed(shot) == Seen::Predicted, "its confirmation only corrects it");
    check(d.confirmed(shot) == Seen::Duplicate, "a second copy is ignored");
    const ShotKey other{8, 1, 100, 0};
    check(d.confirmed(other) == Seen::New, "someone else's shot is new");
    check(d.confirmed(other) == Seen::Duplicate, "and shown once");
    check(d.confirmed({7, 1, 100, 1}) == Seen::New, "another ordinal in the same tick is another shot");
    check(d.confirmed({7, 2, 100, 0}) == Seen::New, "so is the same tick in a new life");
    // A prediction a tick off the host's.
    check(d.predicted({7, 1, 120, 0}), "a prediction at 120");
    check(d.confirmed({7, 1, 121, 0}, 2) == Seen::Predicted, "matches the host's 121 within the window");
    check(d.confirmed({7, 1, 121, 0}, 2) == Seen::Duplicate, "and its copy is a duplicate");
    check(d.predicted({7, 1, 140, 0}), "a prediction at 140");
    check(d.confirmed({7, 1, 145, 0}, 2) == Seen::New, "outside the window it's a different shot");
    // The nearest of two predictions.
    check(d.predicted({9, 1, 200, 0}) && d.predicted({9, 1, 203, 0}), "two predictions");
    check(d.confirmed({9, 1, 202, 0}, 3) == Seen::Predicted, "the host's 202");
    check(d.confirmed({9, 1, 200, 0}, 3) == Seen::Predicted, "matched 203, so 200 is still there for its own");
    // A full ring forgets the oldest.
    for (uint32_t i = 0; i < PresentationDedup::kCapacity; ++i) d.confirmed({100, 0, i, 0});
    check(d.confirmed(other) == Seen::New, "after kCapacity shots the oldest is forgotten");
    d.clear();
    check(d.predicted(shot), "clear forgets everything");
}

// ---- Circuits ------------------------------------------------------------------------------

std::string scopeProblems(std::string_view text, CircuitExecution execution) {
    CircuitDocument document;
    std::string error;
    if (!parseCircuitText(text, "scope.swcircuit", document, error)) return "parse: " + error;
    std::vector<CircuitProblem> problems;
    const auto program = compileCircuit(document, "scope.swcircuit", CircuitKind::Entity, problems);
    if (!program) return "compile: " + (problems.empty() ? std::string() : problems[0].message);
    std::string all;
    for (const auto& p : circuitScopeProblems(*program, execution)) all += p.message + "\n";
    return all;
}

void write(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

const char* kHurt = R"(shockwave_circuit 1
node start Event.BeginPlay
node hurt Component.Health.Set
  exec start.then
  in current = 25
)";

const char* kAskHost = R"(shockwave_circuit 1
node start Event.BeginPlay
node ask Network.Command.test.ask
  exec start.then
  in level = 2
)";

const char* kCountBooms = R"(shockwave_circuit 1
var booms Int 0
var power Int 0
node boom Network.Event.test.boom
node add Math.AddInt
  in a = booms
  in b = 1
node keep Set.booms
  exec boom.then
  in value = add.result
node last Set.power
  exec keep.then
  in value = boom.power
)";

void circuitScopes() {
    registerNetworkNodes(registry());
    check(findNode("Network.Command.test.ask") != nullptr, "the registry's command has a node");
    check(findNode("Network.Event.test.boom") != nullptr, "and its event");
    check(findNode("Network.Command.test.ask")->capability == NodeCapability::NetworkRequest, "a command is a request");
    check(findNode("Component.Health.Set")->capability == NodeCapability::Authority, "Set changes the game");
    check(findNode("Component.Health.Get")->capability == NodeCapability::Presentation, "Get only reads");
    check(scopeProblems(kHurt, CircuitExecution::Authority).empty(), "an Authority circuit may change the game");
    check(
        scopeProblems(kHurt, CircuitExecution::LocalOwner).find("Component.Health.Set") != std::string::npos,
        "a LocalOwner circuit may not (" + scopeProblems(kHurt, CircuitExecution::LocalOwner) + ")");
    check(!scopeProblems(kHurt, CircuitExecution::Presentation).empty(), "nor a Presentation circuit");
    check(scopeProblems(kAskHost, CircuitExecution::LocalOwner).empty(), "a LocalOwner circuit may ask the host");
    check(!scopeProblems(kAskHost, CircuitExecution::Presentation).empty(), "a Presentation circuit may not");
    check(scopeProblems(kCountBooms, CircuitExecution::Presentation).empty(), "anyone may hear the host's events");
}

void circuitsAtRunTime(const fs::path& content) {
    write(content / "Circuits/hurt.swcircuit", kHurt);
    write(content / "Circuits/booms.swcircuit", kCountBooms);
    circuitLibrary().clear();

    // On a client: neither the host's circuit nor an owner's circuit that changes health runs.
    Session s;
    Machine& host = s.add(NetworkRole::Authority);
    Machine& a = s.add(NetworkRole::Client);
    check(s.until([&] { return a.net->state() == NetworkState::Active; }, 300), "the client becomes active");
    auto pawn = host.net->spawn(kPawn, {}, a.net->localPlayer());
    s.run(20);
    const Entity owned = a.net->entity(host.net->networkId(pawn.value()));
    check(owned.valid() && a.net->isLocallyControlled(owned), "the client owns its pawn");
    a.world.add<Health>(owned);
    a.world.add<Circuit>(owned, {"Circuits/hurt.swcircuit", true, CircuitExecution::LocalOwner});
    const Entity authority = a.world.create();
    a.world.add<Health>(authority);
    a.world.add<Circuit>(authority, {"Circuits/hurt.swcircuit", true, CircuitExecution::Authority});
    const Entity listener = a.world.create();
    a.world.add<Circuit>(listener, {"Circuits/booms.swcircuit", true, CircuitExecution::Presentation});
    CircuitHost clientHost;
    clientHost.network = a.net.get();
    CircuitWorld circuits(a.world, clientHost);
    circuits.beginPlay();
    check(a.world.get<Health>(owned)->current == 100, "an unauthorised circuit can't apply damage on a client");
    check(a.world.get<Health>(authority)->current == 100, "the host's circuit doesn't run on a client");
    bool reported = false;
    for (const auto& d : circuits.diagnostics())
        reported = reported ||
                   (d.find("Component.Health.Set") != std::string::npos && d.find("doesn't run") != std::string::npos);
    check(reported, "and says why");

    // On the host, the Authority circuit does.
    host.world.add<Health>(pawn.value());
    host.world.add<Circuit>(pawn.value(), {"Circuits/hurt.swcircuit", true, CircuitExecution::Authority});
    CircuitHost hostHost;
    hostHost.network = host.net.get();
    CircuitWorld hostCircuits(host.world, hostHost);
    hostCircuits.beginPlay();
    check(host.world.get<Health>(pawn.value())->current == 25, "the host's circuit applies damage");

    // The host's events reach the client's circuit once each.
    for (int i = 1; i <= 3; ++i) (void)host.net->sendEvent(kBoom, kNullEntity, power(i));
    s.run(20);
    circuits.fixedTick(1.0 / 60);
    const Value* booms = circuits.variable(listener, "booms");
    check(booms && booms->i == 3, "three events run the handler three times");
    check(circuits.variable(listener, "power")->i == 3, "with each event's payload");
    s.run(5);
    circuits.fixedTick(1.0 / 60);
    check(circuits.variable(listener, "booms")->i == 3, "and not again");
    circuits.endPlay();
    hostCircuits.endPlay();

    // A scripted host sends an event twice: the handler runs once.
    Scripted t;
    World& w = t.world;
    const Entity heard = w.create();
    w.add<Circuit>(heard, {"Circuits/booms.swcircuit", true, CircuitExecution::Presentation});
    CircuitHost scriptedHost;
    scriptedHost.network = t.client.get();
    CircuitWorld scripted(w, scriptedHost);
    scripted.beginPlay();
    t.host.event(1, 5);
    t.host.event(1, 5);
    for (int i = 0; i < 5; ++i) t.step();
    scripted.fixedTick(1.0 / 60);
    check(scripted.variable(heard, "booms")->i == 1, "a duplicated event packet runs its handler once");
    scripted.endPlay();
}

}   // namespace

int main() {
    registerCircuitComponents();
    registerNetworkComponents();
    registerComponent<Health>("Health", {field("current", &Health::current)});
    const fs::path root =
        fs::temp_directory_path() /
        ("shockwave-combat-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path content = root / "Content";
    fs::create_directories(content);
    Assets assets;
    prefabs().setup(content, &assets);

    rewind();
    events();
    duplicateEvents();
    dedup();
    circuitScopes();
    circuitsAtRunTime(content);

    std::error_code ec;
    fs::remove_all(root, ec);
    if (failures == 0) std::printf("Network combat: %d checks passed\n", checks);
    return failures == 0 ? 0 : 1;
}
