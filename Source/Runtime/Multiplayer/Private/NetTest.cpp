// player --net-test scenarios: NetworkWorlds on a LoopbackHub (Multiplayer/NetTest.h).
#include <Multiplayer/NetTest.h>

#include <Multiplayer/NetworkSchema.h>
#include <Multiplayer/Replication.h>

#include <Core/Jobs.h>
#include <Engine/Components.h>
#include <Engine/PhysicsComponents.h>
#include <Network/LoopbackServices.h>
#include <Physics/Physics.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <numeric>

namespace shockwave::net {

namespace {

constexpr const char* kScenarios[] = {"handshake",  "rejections",   "stop-start", "replication",
                                      "prediction", "map_rotation", "match"};
constexpr uint64_t kHostIdentity = 1000;

struct Node {
    std::string name;
    std::unique_ptr<LoopbackOnlineServices> services;
    std::unique_ptr<World> scene;   // replication: this machine's world
    std::unique_ptr<PhysicsWorld> physics;   // prediction: this machine's physics
    std::unique_ptr<NetworkWorld> world;
    ReasonCode expected = ReasonCode::Normal;
    double joinMs = -1;
    uint64_t stepNanos = 0;   // wall-clock time of its last step()
    NetTestPeer result;
};

// Host and clients stepped together on the hub's virtual clock, one tick at a time.
class Harness {
public:
    Harness(const NetTestOptions& options, const FaultProfile& faults) : m_options(options) {
        LoopbackHubConfig config;
        config.seed = options.seed;
        config.faults = faults;
        m_hub = std::make_unique<LoopbackHub>(config);
        DeterministicRandom random(options.seed ^ 0x5eedu);
        for (uint8_t& b : m_compatibility.schemaDigest) b = uint8_t(random.next());
        for (uint8_t& b : m_compatibility.contentDigest) b = uint8_t(random.next());
        m_compatibility.gameProtocol = "nettest";
        m_compatibility.sceneId = 1;
    }

    NetworkStart start(NetworkRole role) const {
        NetworkStart s;
        s.role = role;
        s.settings = m_options.settings;
        s.compatibility = m_compatibility;
        s.address = {TransportAddress::Loopback, 1, 0};
        s.registry = m_registry;
        s.prefabs = m_prefabs;
        s.spawnPrefab = m_spawn;
        return s;
    }

    // Replication: every node gets a world made by makeScene, and sessions use this
    // registry (whose digest they compare) and prefab list.
    void replicate(
        const ReplicationRegistry& registry, std::vector<NetworkPrefab> prefabs, NetworkSpawnFn spawn,
        std::function<void(World&, uint32_t node)> makeScene) {
        m_registry = &registry;
        m_compatibility.schemaDigest = registry.schemaDigest();
        m_prefabs = std::move(prefabs);
        m_spawn = std::move(spawn);
        m_makeScene = std::move(makeScene);
    }
    // Called on the host between drain and endTick, once per tick.
    std::function<void(uint32_t tick)> onHostTick;
    // Called on every node after onHostTick, before endTick.
    std::function<void(Node&, uint32_t tick)> onTick;

    Node& add(std::string name, const NetworkStart& start, uint64_t identity) {
        Node& node = m_nodes.emplace_back();
        node.name = std::move(name);
        node.services = std::make_unique<LoopbackOnlineServices>(*m_hub, PeerId{identity});
        if (m_makeScene) {
            node.scene = std::make_unique<World>();
            m_makeScene(*node.scene, uint32_t(m_nodes.size() - 1));
        }
        node.world = std::make_unique<NetworkWorld>(start, node.services.get(), node.scene.get());
        node.result.name = node.name;
        node.result.role = start.role;
        node.result.peer = identity;
        return node;
    }

    // Starts every node not started yet; false (with the error) when one can't.
    bool begin(Node& node, std::vector<std::string>& errors) {
        if (auto begun = node.world->begin(); !begun) {
            errors.push_back(node.name + ": didn't start: " + begun.error().message);
            return false;
        }
        return true;
    }

    double nowSeconds() const { return double(m_hub->nowUs()) / 1e6; }

    void step() {
        const double now = nowSeconds();
        for (Node& node : m_nodes) {
            if (!node.world) continue;
            const auto began = std::chrono::steady_clock::now();
            node.services->pump();
            OnlineEvent event;
            while (node.services->pollEvent(event)) node.world->handleOnlineEvent(event);
            node.world->pump(now);
            node.world->drain(m_ticks);
            if (node.world->role() == NetworkRole::Authority && onHostTick) onHostTick(m_ticks);
            if (onTick) onTick(node, m_ticks);
            node.world->endTick(m_ticks);
            node.stepNanos = uint64_t(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count());
            if (node.world->role() == NetworkRole::Client && node.joinMs < 0 &&
                node.world->state() == NetworkState::Active)
                node.joinMs = now * 1000;
        }
        m_hub->advance(1000000 / std::max<uint32_t>(m_options.settings.tickRate, 1));
        ++m_ticks;
    }

    bool runUntil(const std::function<bool()>& done, uint32_t maxTicks) {
        for (uint32_t i = 0; i < maxTicks; ++i) {
            if (done()) return true;
            step();
        }
        return done();
    }

    // Records where each node got to, then stops it.
    void record(Node& node) {
        NetworkWorld& w = *node.world;
        node.result.state = w.state();
        node.result.reason = w.state() == NetworkState::Failed ? w.failure() : ReasonCode::Normal;
        node.result.expected = node.expected;
        node.result.slot = w.localSlot();
        node.result.joinMs = node.joinMs;
        const NetworkWorldStats& s = w.stats();
        m_sent += s.messagesSent;
        m_received += s.messagesReceived;
        m_rejected += s.rejected;
        m_malformed += s.malformed;
        m_replication.objects = std::max<uint64_t>(m_replication.objects, s.objects);
        m_replication.spawns += w.role() == NetworkRole::Authority ? s.spawns : 0;
        m_replication.despawns += w.role() == NetworkRole::Authority ? s.despawns : 0;
        m_replication.snapshotsSent += s.snapshotsSent;
        m_replication.snapshotPartsSent += s.snapshotPartsSent;
        m_replication.recordsApplied += s.recordsApplied;
        m_replication.staleRecords += s.staleRecords;
        m_replication.heldSnapshots += s.heldSnapshots;
        m_replication.expiredSnapshots += s.expiredSnapshots;
        m_replication.resyncRequests += w.role() == NetworkRole::Client ? s.resyncRequests : 0;
        m_replication.controlQueueHigh = std::max(m_replication.controlQueueHigh, s.controlQueueHigh);
    }
    // Map rotation: the node's session ends and the next one starts from `start`.
    bool restart(Node& node, const NetworkStart& start, std::vector<std::string>& errors) {
        node.world->close();
        node.world = std::make_unique<NetworkWorld>(start, node.services.get(), node.scene.get());
        node.joinMs = -1;
        return begin(node, errors);
    }
    void stop(Node& node) {
        if (!node.world) return;
        node.world->close();
        node.world.reset();
    }
    // Stops everyone, lets the hub settle, and reports what's still open.
    void stopAll(NetTestReport& report) {
        for (Node& node : m_nodes)
            if (node.world) record(node), stop(node);
        for (int i = 0; i < 10; ++i) step();
        finish(report);
    }
    void finish(NetTestReport& report) {
        report.packetsInFlightAfterStop += m_hub->stats().packetsInFlight;
        for (Node& node : m_nodes) {
            report.openAuthSessionsAfterStop += node.services->openAuthSessions();
            report.liveTicketsAfterStop += node.services->liveTickets();
            report.peers.push_back(node.result);
        }
        report.messagesSent += m_sent;
        report.messagesReceived += m_received;
        report.rejected += m_rejected;
        report.malformed += m_malformed;
        report.durationTicks += m_ticks;
        report.replication = m_replication;
    }

    std::deque<Node>& nodes() { return m_nodes; }
    LoopbackHub& hub() { return *m_hub; }
    uint32_t ticks() const { return m_ticks; }
    NetworkCompatibility& compatibility() { return m_compatibility; }
    uint32_t ticksFor(double seconds) const { return uint32_t(seconds * m_options.settings.tickRate) + 1; }

private:
    NetTestOptions m_options;
    std::unique_ptr<LoopbackHub> m_hub;
    std::deque<Node> m_nodes;   // the host first; a deque, so references stay valid
    NetworkCompatibility m_compatibility;
    const ReplicationRegistry* m_registry = nullptr;
    std::vector<NetworkPrefab> m_prefabs;
    NetworkSpawnFn m_spawn;
    std::function<void(World&, uint32_t)> m_makeScene;
    uint32_t m_ticks = 0;
    uint64_t m_sent = 0, m_received = 0, m_rejected = 0, m_malformed = 0;
    NetTestReplication m_replication;
};

void checkClean(NetTestReport& report) {
    if (report.packetsInFlightAfterStop)
        report.errors.push_back(
            std::to_string(report.packetsInFlightAfterStop) + " packets still in flight after stop");
    if (report.openAuthSessionsAfterStop)
        report.errors.push_back(std::to_string(report.openAuthSessionsAfterStop) + " auth sessions left open");
    if (report.liveTicketsAfterStop)
        report.errors.push_back(std::to_string(report.liveTicketsAfterStop) + " session tickets not cancelled");
}

void handshake(const NetTestOptions& options, const FaultProfile& faults, NetTestReport& report) {
    Harness h(options, faults);
    Node& host = h.add("host", h.start(NetworkRole::Authority), kHostIdentity);
    if (!h.begin(host, report.errors)) return h.finish(report);
    for (uint32_t i = 0; i < options.clients; ++i) {
        Node& client = h.add("client " + std::to_string(i + 1), h.start(NetworkRole::Client), kHostIdentity + 1 + i);
        h.begin(client, report.errors);
    }
    auto joined = [&] {
        for (Node& n : h.nodes())
            if (n.world->role() == NetworkRole::Client && n.world->state() != NetworkState::Active) return false;
        return true;
    };
    if (!h.runUntil(joined, h.ticksFor(options.settings.handshakeTimeoutSeconds + 1)))
        report.errors.push_back("not every client reached active");
    const NetworkWorldStats& hs = h.nodes()[0].world->stats();
    if (hs.admitted != options.clients)
        report.errors.push_back(
            "the host admitted " + std::to_string(hs.admitted) + " of " + std::to_string(options.clients) + " clients");
    std::vector<uint16_t> slots;
    for (Node& n : h.nodes())
        if (n.world->role() == NetworkRole::Client) slots.push_back(n.world->localSlot());
    std::sort(slots.begin(), slots.end());
    for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i] != i + 2) {
            report.errors.push_back("player slots aren't 2.." + std::to_string(options.clients + 1));
            break;
        }
    h.stopAll(report);
    checkClean(report);
}

struct Rejection {
    const char* name;
    ReasonCode expected;
    std::function<void(Harness&, NetworkStart& client)> setup;
    double seconds = 3;
    bool hostStops = false;   // the host closes once the client is authenticating
};

void rejections(const NetTestOptions& options, NetTestReport& report) {
    NetTestOptions o = options;
    o.settings.maxPlayers = 2;
    const Rejection cases[] = {
        {"protocol mismatch", ReasonCode::ProtocolMismatch,
         [](Harness&, NetworkStart& c) { c.compatibility.gameProtocol = "another game"; }},
        {"content mismatch", ReasonCode::ContentMismatch,
         [](Harness&, NetworkStart& c) { c.compatibility.contentDigest[0] ^= 1; }},
        {"schema mismatch", ReasonCode::ContentMismatch,
         [](Harness&, NetworkStart& c) { c.compatibility.schemaDigest[31] ^= 1; }},
        {"scene mismatch", ReasonCode::MapNotLoaded, [](Harness&, NetworkStart& c) { c.compatibility.sceneId = 2; }},
        {"server full", ReasonCode::ServerFull, nullptr},
        {"ticket refused", ReasonCode::AuthenticationFailed,
         [](Harness& h, NetworkStart&) {
             h.nodes()[0].services->setValidation(LoopbackOnlineServices::Validation::Refuse);
         }},
        {"ticket never answered", ReasonCode::AuthUnavailable,
         [](Harness& h, NetworkStart&) {
             h.nodes()[0].services->setValidation(LoopbackOnlineServices::Validation::NoAnswer);
         },
         o.settings.handshakeTimeoutSeconds + 2},
        {"host stops mid-handshake", ReasonCode::HostLeft, nullptr, 3, true},
        {"no host", ReasonCode::ServiceUnavailable, nullptr},
    };
    for (const Rejection& c : cases) {
        Harness h(o, {});
        const std::string name = c.name;
        Node& host = h.add("host (" + name + ")", h.start(NetworkRole::Authority), kHostIdentity);
        NetworkStart start = h.start(NetworkRole::Client);
        if (c.setup) c.setup(h, start);
        if (name == "no host") {
            host.world.reset();   // nobody listens
        } else if (!h.begin(host, report.errors)) {
            h.finish(report);
            continue;
        }
        if (name == "server full") {   // one client fills the only slot first
            Node& first = h.add("first client", h.start(NetworkRole::Client), kHostIdentity + 1);
            h.begin(first, report.errors);
            h.runUntil([&] { return first.world->state() == NetworkState::Active; }, h.ticksFor(3));
        }
        Node& client = h.add(name, start, kHostIdentity + 2);
        client.expected = c.expected;
        if (c.hostStops) client.services->setValidation(LoopbackOnlineServices::Validation::NoAnswer);
        h.begin(client, report.errors);
        if (c.hostStops) {
            h.runUntil([&] { return client.world->state() == NetworkState::Authenticating; }, h.ticksFor(3));
            h.runUntil([] { return false; }, 10);   // tickets exchanged; the client is still validating
            h.record(h.nodes()[0]);
            h.stop(h.nodes()[0]);
        }
        h.runUntil([&] { return client.world->state() == NetworkState::Failed; }, h.ticksFor(c.seconds));
        if (client.world->state() != NetworkState::Failed)
            report.errors.push_back(
                name + ": the client didn't fail (it's " + networkStateName(client.world->state()) + ")");
        else if (client.world->failure() != c.expected)
            report.errors.push_back(
                name + ": failed with " + reasonName(client.world->failure()) + ", expected " + reasonName(c.expected));
        h.stopAll(report);
    }
    checkClean(report);
}

void stopStart(const NetTestOptions& options, NetTestReport& report) {
    Harness h(options, {});
    Node& host = h.add("host", h.start(NetworkRole::Authority), kHostIdentity);
    Node& client = h.add("client", h.start(NetworkRole::Client), kHostIdentity + 1);
    for (uint32_t round = 0; round < 100; ++round) {
        host.world = std::make_unique<NetworkWorld>(h.start(NetworkRole::Authority), host.services.get());
        client.world = std::make_unique<NetworkWorld>(h.start(NetworkRole::Client), client.services.get());
        if (!h.begin(host, report.errors) || !h.begin(client, report.errors)) break;
        client.joinMs = -1;
        if (!h.runUntil([&] { return client.world->state() == NetworkState::Active; }, h.ticksFor(5))) {
            report.errors.push_back("round " + std::to_string(round + 1) + ": the client didn't join");
            break;
        }
        // Alternate who stops first; the other sees it, then stops too.
        Node& first = round % 2 ? client : host;
        Node& second = round % 2 ? host : client;
        h.stop(first);
        h.runUntil([] { return false; }, 3);
        if (second.world->role() == NetworkRole::Client && second.world->state() != NetworkState::Failed)
            report.errors.push_back("round " + std::to_string(round + 1) + ": the client didn't notice the host stop");
        if (second.world->role() == NetworkRole::Authority && second.world->stats().admitted != 0)
            report.errors.push_back("round " + std::to_string(round + 1) + ": the host kept a stopped client");
        h.record(second);
        h.stop(second);
        h.runUntil([] { return false; }, 2);
        if (host.services->openAuthSessions() || client.services->openAuthSessions() || host.services->liveTickets() ||
            client.services->liveTickets()) {
            report.errors.push_back("round " + std::to_string(round + 1) + ": auth sessions or tickets left open");
            break;
        }
        ++report.rounds;
    }
    h.finish(report);
    checkClean(report);
}

// A box moving on the host, and crates spawned for its players, moved, respawned and
// despawned, while clients join (one late). Then everything stops moving, and every
// client must have exactly the host's objects.
void replication(const NetTestOptions& options, const FaultProfile& faults, NetTestReport& report) {
    ReplicationRegistry registry;
    registerEngineReplication(registry);
    std::vector<std::string> problems;
    if (!registry.freeze(problems)) {
        report.errors.push_back("the engine registry didn't freeze");
        return;
    }
    Harness h(options, faults);
    auto spawnCrate = [](World& world, const NetworkPrefab& prefab, const Transform& at) {
        const Entity e = world.create();
        world.add<Transform>(e, at);
        world.add<Name>(e, {"crate"});
        NetworkObject n;
        n.profile = prefab.profile;
        world.add<NetworkObject>(e, n);
        return e;
    };
    // Each machine builds the scene in its own order, with its own unrelated entities, so
    // local entity handles differ: only keys bind authored objects.
    auto makeScene = [](World& world, uint32_t node) {
        for (uint32_t i = 0; i < node % 3; ++i) world.add<Name>(world.create(), {"filler"});
        const char* keys[] = {"box", "lamp"};
        for (uint32_t k = 0; k < 2; ++k) {
            const char* key = keys[(k + node) % 2];
            const Entity e = world.create();
            world.add<Transform>(e, {});
            NetworkObject n;
            n.key = key;
            n.profile = "engine.pose.v1";
            n.mode = std::string_view(key) == "lamp" ? NetworkObject::Mode::Static : NetworkObject::Mode::Interpolated;
            world.add<NetworkObject>(e, n);
        }
    };
    h.replicate(registry, {{1, "Prefabs/Net Crate.swprefab", "engine.pose.v1"}}, spawnCrate, makeScene);
    Node& host = h.add("host", h.start(NetworkRole::Authority), kHostIdentity);
    if (!h.begin(host, report.errors)) return h.finish(report);
    World& hostWorld = *host.scene;
    Entity box = kNullEntity;
    hostWorld.each<const NetworkObject>([&](Entity e, const NetworkObject& n) {
        if (n.key == "box") box = e;
    });
    const uint32_t early = options.clients > 1 ? options.clients - 1 : options.clients;
    for (uint32_t i = 0; i < early; ++i) {
        Node& client = h.add("client " + std::to_string(i + 1), h.start(NetworkRole::Client), kHostIdentity + 1 + i);
        h.begin(client, report.errors);
    }
    const uint32_t rate = std::max<uint32_t>(options.settings.tickRate, 1);
    const uint32_t lateTick = h.ticks() + 4 * rate, quietTick = h.ticks() + 8 * rate, endTick = quietTick + 3 * rate;
    std::deque<Entity> crates;
    bool moving = true;
    h.onHostTick = [&](uint32_t tick) {
        if (!moving) return;
        const float t = float(tick) / float(rate);
        if (Transform* b = hostWorld.get<Transform>(box)) {
            b->position = {5.0f * std::sin(t), 1.0f, 0.0f};
            b->rotation = glm::angleAxis(t, glm::vec3(0, 1, 0));
        }
        for (Entity c : crates)
            if (Transform* ct = hostWorld.get<Transform>(c)) ct->position.y += 0.01f;
        NetworkWorld& net = *host.world;
        if (tick % (rate / 2) == 0) {   // a crate for each player in turn
            const uint16_t owner = uint16_t(2 + (tick / (rate / 2)) % std::max<uint32_t>(net.stats().admitted, 1));
            Transform at;
            at.position = {float(tick % 7), 0.0f, float(tick % 5)};
            if (auto crate = net.spawn(1, at, net.stats().admitted ? owner : 0))
                crates.push_back(crate.value());
            else
                report.errors.push_back(std::string("spawn failed: ") + crate.error().message);
        }
        if (crates.size() > 4) {   // the oldest goes, alternately despawned and just destroyed
            if (tick % 2)
                (void)net.despawn(crates.front());
            else
                hostWorld.destroy(crates.front());
            crates.pop_front();
        }
        if (tick % rate == 0 && !crates.empty()) {   // a teleport: a new incarnation
            Transform at;
            at.position = {0.0f, 10.0f, 0.0f};
            (void)net.respawn(crates.back(), &at);
        }
    };
    h.runUntil([&] { return h.ticks() >= lateTick; }, lateTick + 1);
    if (options.clients > 1) {
        Node& late = h.add(
            "client " + std::to_string(options.clients) + " (late)", h.start(NetworkRole::Client),
            kHostIdentity + options.clients);
        h.begin(late, report.errors);
    }
    h.runUntil([&] { return h.ticks() >= quietTick; }, quietTick + 1);
    moving = false;
    h.runUntil([&] { return h.ticks() >= endTick; }, endTick + 1);

    // Every client has exactly the host's objects, owners, incarnations and poses.
    NetworkWorld& net = *host.world;
    for (Node& n : h.nodes()) {
        if (!n.world || n.world->role() != NetworkRole::Client) continue;
        if (n.world->state() != NetworkState::Active) {
            report.errors.push_back(n.name + " isn't active (" + networkStateName(n.world->state()) + ")");
            continue;
        }
        World& world = *n.scene;
        if (n.world->objectCount() != net.objectCount())
            report.errors.push_back(
                n.name + " has " + std::to_string(n.world->objectCount()) + " network objects, the host " +
                std::to_string(net.objectCount()));
        size_t replicas = 0;
        world.each<const NetworkIdentity>([&](Entity, const NetworkIdentity&) { ++replicas; });
        if (replicas != net.objectCount())
            report.errors.push_back(n.name + " has " + std::to_string(replicas) + " replicated entities");
        hostWorld.each<const NetworkIdentity, const Transform>(
            [&](Entity, const NetworkIdentity& id, const Transform& t) {
                const Entity e = n.world->entity(id.id);
                const NetworkIdentity* theirs = n.world->identity(e);
                const Transform* tt = world.get<Transform>(e);
                if (!theirs || !tt) {
                    report.errors.push_back(n.name + " lacks network object " + std::to_string(id.id));
                    return;
                }
                if (theirs->ownerSlot != id.ownerSlot || theirs->incarnation != id.incarnation)
                    report.errors.push_back(n.name + " disagrees on the owner of " + std::to_string(id.id));
                const double error = glm::length(tt->position - t.position);
                report.replication.maxPositionError = std::max(report.replication.maxPositionError, error);
            });
    }
    if (report.replication.maxPositionError > 1e-4)
        report.errors.push_back("a client's pose differs from the host's after traffic settled");
    const double positionError = report.replication.maxPositionError;
    h.stopAll(report);
    report.replication.maxPositionError = positionError;
    checkClean(report);
}

// The scripted walk: each client turns through four directions every second and jumps
// every 1.5 s, until `quiet`, then stands still.
InputCommand scripted(uint32_t client, uint32_t tick, uint32_t rate, bool quiet) {
    InputCommand c;
    if (quiet) return c;
    static constexpr int8_t kDirections[4][2] = {{127, 0}, {0, 127}, {-127, 0}, {0, -127}};
    const uint32_t d = (tick / rate + client) % 4;
    c.moveX = kDirections[d][0];
    c.moveZ = kDirections[d][1];
    if ((tick + client * 7) % (rate * 3 / 2) == 0) c.pressedBits = InputCommand::kPressedJump;
    if ((tick / rate) % 3 == 2) c.heldBits = InputCommand::kHeldSprint;
    return c;
}

// The scenario's movement rule, the same on the host and in prediction.
PredictedMove scriptedMove(const InputCommand& c) {
    PredictedMove m;
    const float speed = (c.heldBits & InputCommand::kHeldSprint) ? 7.0f : 4.0f;
    m.desiredVelocity = glm::vec3(float(c.moveX) / 127.0f, 0.0f, float(c.moveZ) / 127.0f) * speed;
    m.jump = (c.pressedBits & InputCommand::kPressedJump) != 0;
    return m;
}

void prediction(const NetTestOptions& options, const FaultProfile& faults, NetTestReport& report) {
    ReplicationRegistry registry;
    registerEngineReplication(registry);
    std::vector<std::string> problems;
    if (!registry.freeze(problems)) {
        report.errors.push_back("the engine registry didn't freeze");
        return;
    }
    JobSystem jobs(1);
    Harness h(options, faults);
    auto spawnCharacter = [](World& world, const NetworkPrefab& prefab, const Transform& at) {
        const Entity e = world.create();
        world.add<Transform>(e, at);
        world.add<Name>(e, {"character"});
        world.add<CharacterController>(e, {});
        NetworkObject n;
        n.profile = prefab.profile;
        n.mode = NetworkObject::Mode::PredictedCharacter;
        world.add<NetworkObject>(e, n);
        return e;
    };
    // A floor and a wall, both static: what predicted characters collide with.
    auto makeScene = [](World& world, uint32_t) {
        auto block = [&](const char* name, glm::vec3 position, glm::vec3 halfExtents) {
            const Entity e = world.create();
            world.add<Name>(e, {name});
            Transform t;
            t.position = position;
            world.add<Transform>(e, t);
            Collider c;
            c.halfExtents = halfExtents;
            world.add<Collider>(e, c);
            RigidBody body;
            body.motion = RigidBody::Motion::Static;
            world.add<RigidBody>(e, body);
        };
        block("floor", {0, -0.5f, 0}, {40, 0.5f, 40});
        block("wall", {3, 1, 0}, {0.25f, 1, 4});
        // A trigger the characters walk through: the host gets its events, a client's
        // prediction and replays must not.
        const Entity trigger = world.create();
        world.add<Name>(trigger, {"trigger"});
        world.add<Transform>(trigger, {});
        Collider c;
        c.halfExtents = {1.5f, 1, 1.5f};
        world.add<Collider>(trigger, c);
        RigidBody sensor;
        sensor.motion = RigidBody::Motion::Static;
        sensor.sensor = true;
        world.add<RigidBody>(trigger, sensor);
    };
    h.replicate(registry, {{1, "Prefabs/Net Character.swprefab", "engine.pose.v1"}}, spawnCharacter, makeScene);
    const uint32_t rate = std::max<uint32_t>(options.settings.tickRate, 1);
    // Per client: game-code calls (the move rule), contact events seen, and when its
    // newest correction happened.
    struct ClientWatch {
        uint64_t moveCalls = 0, contacts = 0, corrections = 0;
        double correctedAt = -1;
        float maxOffset = 0;
    };
    std::deque<ClientWatch> watch;
    uint64_t hostContacts = 0;
    auto join = [&](Node& node) {
        node.physics = std::make_unique<PhysicsWorld>(*node.scene, jobs);
        node.world->setPhysics(node.physics.get());
        if (node.world->role() == NetworkRole::Client) {
            ClientWatch& w = watch.emplace_back();
            node.world->setPredictionMove([&w](const World&, Entity, const InputCommand& c) {
                ++w.moveCalls;
                return scriptedMove(c);
            });
        }
        return h.begin(node, report.errors);
    };
    Node& host = h.add("host", h.start(NetworkRole::Authority), kHostIdentity);
    if (!join(host)) return h.finish(report);
    for (uint32_t i = 0; i < options.clients; ++i) {
        Node& client = h.add("client " + std::to_string(i + 1), h.start(NetworkRole::Client), kHostIdentity + 1 + i);
        join(client);
    }
    World& hostWorld = *host.scene;
    NetworkWorld& hostNet = *host.world;
    std::vector<std::pair<uint16_t, Entity>> pawns;   // host: by owner slot
    const uint32_t quietTick = h.ticks() + 12 * rate, endTick = quietTick + 3 * rate;
    h.onHostTick = [&](uint32_t) {
        NetworkEvent event;
        while (hostNet.pollEvent(event)) {
            if (event.kind != NetworkEventKind::PlayerJoined) continue;
            Transform at;
            at.position = {float(pawns.size() % 4) * 1.5f - 2.0f, 0.05f, float(pawns.size() / 4) * 1.5f - 2.0f};
            if (auto pawn = hostNet.spawn(1, at, event.slot))
                pawns.emplace_back(event.slot, pawn.value());
            else
                report.errors.push_back(std::string("spawn failed: ") + pawn.error().message);
        }
        for (const auto& [slot, pawn] : pawns) {
            if (!hostWorld.alive(pawn)) continue;
            InputCommand c;
            hostNet.takeInput(slot, hostNet.identity(pawn) ? hostNet.identity(pawn)->incarnation : 0, c);
            const PredictedMove move = scriptedMove(c);
            CharacterController& controller = *hostWorld.get<CharacterController>(pawn);
            controller.desiredVelocity = move.desiredVelocity;
            controller.jumpRequested = controller.jumpRequested || move.jump;
        }
    };
    h.onTick = [&](Node& node, uint32_t tick) {
        NetworkWorld& net = *node.world;
        if (net.role() == NetworkRole::Client && net.state() == NetworkState::Active) {
            const Entity mine = net.controlledEntity().valid() ? net.controlledEntity() : kNullEntity;
            const NetworkIdentity* identity = net.identity(mine);
            uint32_t client = 0;
            for (uint32_t i = 0; i < h.nodes().size(); ++i)
                if (&h.nodes()[i] == &node) client = i;
            InputCommand c = scripted(client, tick, rate, tick >= quietTick);
            c.incarnation = identity ? identity->incarnation : 0;
            if (identity) net.sendInput(c);
        }
        net.prepareStep();
        node.physics->step(1.0 / double(rate));
        if (net.role() == NetworkRole::Authority) {
            hostContacts += node.physics->contacts().size();
            return;
        }
        // A client after its tick: contacts, the correction offset's decay, presentation.
        uint32_t index = 0;
        for (uint32_t i = 1; i < h.nodes().size(); ++i)
            if (&h.nodes()[i] == &node) index = i - 1;
        ClientWatch& w = watch[index];
        w.contacts += node.physics->contacts().size();
        const double now = h.nowSeconds();
        if (net.stats().corrections != w.corrections) {
            w.corrections = net.stats().corrections;
            w.correctedAt = now;
        }
        const float offset = glm::length(net.correctionOffset());
        w.maxOffset = std::max(w.maxOffset, offset);
        if (w.correctedAt >= 0 && now - w.correctedAt > NetworkWorld::kOffsetMaxSeconds + 1e-6 && offset > 0)
            report.errors.push_back(node.name + "'s correction offset outlived 150 ms");
        net.present(1.0f);
    };
    h.runUntil([&] { return h.ticks() >= endTick; }, endTick + 1);

    NetTestPrediction& p = report.prediction;
    p.hostFallbacks = hostNet.stats().inputFallbacks;
    for (Node& n : h.nodes()) {
        if (!n.world || n.world->role() != NetworkRole::Client) continue;
        const NetworkWorldStats& s = n.world->stats();
        p.predictedTicks += s.predictedTicks;
        p.checks += s.predictionChecks;
        p.matches += s.predictionMatches;
        p.corrections += s.corrections;
        p.replayedTicks += s.replayedTicks;
        p.resets += s.predictionResets;
        p.catchUpInputs += s.catchUpInputs;
        p.maxCorrection = std::max(p.maxCorrection, double(s.maxCorrection));
        if (n.world->state() != NetworkState::Active) {
            report.errors.push_back(n.name + " isn't active (" + networkStateName(n.world->state()) + ")");
            continue;
        }
        if (!n.world->predicting()) {
            report.errors.push_back(n.name + " isn't predicting its character");
            continue;
        }
        const Entity mine = n.world->controlledEntity();
        const Entity theirs = hostNet.entity(n.world->networkId(mine));
        const Transform* a = n.scene->get<Transform>(mine);
        const Transform* b = hostWorld.get<Transform>(theirs);
        if (!a || !b) {
            report.errors.push_back(n.name + "'s character is missing on one side");
            continue;
        }
        p.finalError = std::max(p.finalError, double(glm::length(a->position - b->position)));
        if (glm::length(n.world->correctionOffset()) > 1e-4f)
            report.errors.push_back(n.name + " still has a correction offset after standing still");
        if (s.predictedTicks == 0) report.errors.push_back(n.name + " predicted nothing");
        // Every character is drawn where the host has it once everyone stands still: the
        // own one through prediction, the others through interpolation.
        for (const auto& [slot, pawn] : pawns) {
            const Entity e = n.world->entity(hostNet.networkId(pawn));
            const PresentationTransform* shown = n.scene->get<PresentationTransform>(e);
            const Transform* host = hostWorld.get<Transform>(pawn);
            if (!shown || !host) {
                report.errors.push_back(n.name + " doesn't present player " + std::to_string(slot) + "'s character");
                continue;
            }
            p.finalPresentationError =
                std::max(p.finalPresentationError, double(glm::length(shown->value.position - host->position)));
        }
        if (s.interpolatedObjects + 1 != pawns.size())
            report.errors.push_back(n.name + " interpolates " + std::to_string(s.interpolatedObjects) + " objects");
    }
    for (uint32_t i = 0; i < watch.size(); ++i) {
        const ClientWatch& w = watch[i];
        const NetworkWorld* net = h.nodes()[i + 1].world.get();
        if (net && w.moveCalls != net->stats().predictedTicks)
            report.errors.push_back(
                h.nodes()[i + 1].name + ": game code ran " + std::to_string(w.moveCalls) + " times for " +
                std::to_string(net->stats().predictedTicks) + " predicted ticks (replays must not run it)");
        if (w.contacts) report.errors.push_back(h.nodes()[i + 1].name + " saw contact events while predicting");
        p.maxOffset = std::max(p.maxOffset, double(w.maxOffset));
    }
    p.hostContacts = hostContacts;
    if (p.maxOffset > NetworkWorld::kSnapDistance)
        report.errors.push_back("a correction offset beyond the snap distance was shown");
    if (p.finalPresentationError > 0.01)
        report.errors.push_back("a client draws a character away from where the host has it after standing still");
    if (p.finalError > NetworkWorld::kPositionTolerance + 0.002)
        report.errors.push_back("a client's character isn't where the host has it after standing still");
    if (p.checks && p.matches * 2 < p.checks) report.warnings.push_back("most predictions missed the host's state");
    // Without faults the only misses are ticks whose input didn't reach the host in time
    // (the first ones, before the client measured the host's clock).
    if (options.preset == "clean" && (p.corrections > p.hostFallbacks || p.resets))
        report.errors.push_back("predictions missed on a clean network for a tick the host had the input for");
    const NetTestPrediction kept = p;
    h.stopAll(report);
    report.prediction = kept;
    checkClean(report);
}

void mapRotation(const NetTestOptions& options, const FaultProfile& faults, NetTestReport& report) {
    Harness h(options, faults);
    Node& host = h.add("host", h.start(NetworkRole::Authority), kHostIdentity);
    if (!h.begin(host, report.errors)) return h.finish(report);
    for (uint32_t i = 0; i < options.clients; ++i) {
        Node& client = h.add("client " + std::to_string(i + 1), h.start(NetworkRole::Client), kHostIdentity + 1 + i);
        h.begin(client, report.errors);
    }
    auto everyoneActive = [&](uint32_t epoch) {
        for (Node& n : h.nodes())
            if (n.world->state() != NetworkState::Active || n.world->mapEpoch() != epoch) return false;
        return h.nodes()[0].world->stats().activePeers == options.clients;
    };
    if (!h.runUntil([&] { return everyoneActive(1); }, h.ticksFor(options.settings.handshakeTimeoutSeconds + 1))) {
        report.errors.push_back("not every client reached active");
        h.stopAll(report);
        return;
    }
    std::vector<uint16_t> slots;
    for (Node& n : h.nodes()) slots.push_back(n.world->localSlot());
    DeterministicRandom random(options.seed ^ 0x707a7e);
    NetTestRotation r;
    double totalMs = 0;
    for (uint32_t k = 0; k < options.rotations; ++k) {
        // Each client takes its own (seeded) time to load the next map.
        std::vector<uint32_t> loadTicks;
        for (size_t i = 0; i < h.nodes().size(); ++i) loadTicks.push_back(1 + random.next() % h.ticksFor(0.5));
        MapRotationRequest request;
        request.level = "map-" + std::to_string(k + 2);
        request.sceneId = uint16_t(1 + (k + 1) % 2);
        request.readySeconds = 10;
        const double began = h.nowSeconds();
        if (auto prepared = host.world->prepareRotation(request); !prepared) {
            report.errors.push_back(std::string("the host couldn't announce a map: ") + prepared.error().message);
            break;
        }
        const uint32_t from = h.ticks();
        auto committed = [&] {
            for (Node& n : h.nodes())
                if (n.world->rotation().phase != RotationPhase::Committed) return false;
            return true;
        };
        bool ok = h.runUntil(
            [&] {
                for (size_t i = 0; i < h.nodes().size(); ++i) {
                    NetworkWorld& w = *h.nodes()[i].world;
                    if (w.rotation().phase == RotationPhase::Preparing && h.ticks() - from >= loadTicks[i])
                        w.rotationReady();
                }
                return committed();
            },
            h.ticksFor(request.readySeconds + 2));
        if (!ok) {
            report.errors.push_back("rotation " + std::to_string(k + 1) + " wasn't committed by everyone");
            break;
        }
        // Everyone ends this session and starts the next: the host first, as LevelHost does
        // a frame later on a client that learnt of the commit later.
        std::vector<NetworkStart> next;
        for (Node& n : h.nodes()) next.push_back(n.world->nextStart());
        for (size_t i = 0; i < h.nodes().size(); ++i) h.restart(h.nodes()[i], next[i], report.errors);
        ok = h.runUntil([&] { return everyoneActive(k + 2); }, h.ticksFor(15));
        if (!ok) {
            report.errors.push_back("not everyone rejoined after rotation " + std::to_string(k + 1));
            break;
        }
        for (size_t i = 0; i < h.nodes().size(); ++i)
            if (h.nodes()[i].world->localSlot() != slots[i]) {
                report.errors.push_back(h.nodes()[i].name + " came back in another slot");
                break;
            }
        if (h.nodes()[0].world->start().compatibility.sceneId != request.sceneId)
            report.errors.push_back("the host's next session plays another scene");
        const NetworkWorldStats& hs = h.nodes()[0].world->stats();
        r.slotsReclaimed += hs.slotsReclaimed;
        for (Node& n : h.nodes()) r.rejoinAttempts += n.world->stats().rejoinAttempts;
        const double ms = (h.nowSeconds() - began) * 1000;
        r.maxRotationMs = std::max(r.maxRotationMs, ms);
        totalMs += ms;
        ++r.rotations;
    }
    if (r.rotations) r.meanRotationMs = totalMs / r.rotations;
    if (r.slotsReclaimed != uint64_t(r.rotations) * options.clients)
        report.errors.push_back(
            "the host gave " + std::to_string(r.slotsReclaimed) + " slots back; expected " +
            std::to_string(uint64_t(r.rotations) * options.clients));
    report.rotation = r;
    h.stopAll(report);
    checkClean(report);
}

// ---- match: a measured match at 2 to 64 players ----------------------------------------------

constexpr uint16_t kScoreEvent = 40;   // nettest.score {shooter u8 1..64, victim u8 1..64}
constexpr float kEyeHeight = 1.6f, kChestHeight = 0.9f;

glm::vec3 spawnFor(uint16_t slot) {
    const uint32_t i = slot > 0 ? slot - 1u : 0u;
    return {float(i % 8) * 3.0f - 10.5f, 0.05f, float(i / 8) * 3.0f - 10.5f};
}

// The scenario's aim rule, the same on both sides: yaw 0 looks down -z.
void aimAt(InputCommand& c, glm::vec3 from, glm::vec3 to) {
    glm::vec3 d = to - from;
    const float length = glm::length(d);
    if (length < 1e-3f) return;
    d /= length;
    constexpr float kPi = 3.14159265358979f;
    const float yaw = std::atan2(d.x, -d.z);
    const float pitch = std::asin(std::clamp(d.y, -1.0f, 1.0f));
    c.yaw = uint16_t(std::lround((yaw + kPi) / (2 * kPi) * 65536.0f) & 0xffff);
    c.pitch = int16_t(std::clamp<long>(std::lround(pitch / glm::radians(89.0f) * 32767.0f), -32767, 32767));
}

glm::vec3 aimDirection(const InputCommand& c) {
    constexpr float kPi = 3.14159265358979f;
    const float yaw = float(c.yaw) / 65536.0f * 2 * kPi - kPi;
    const float pitch = float(c.pitch) / 32767.0f * glm::radians(89.0f);
    return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, size_t(p * double(values.size() - 1) + 0.5))];
}

void match(const NetTestOptions& options, const FaultProfile& faults, NetTestReport& report) {
    ReplicationRegistry registry;
    registerEngineReplication(registry);
    std::string why;
    EventDesc score;
    score.id = kScoreEvent;
    score.name = "nettest.score";
    score.payload = {
        netInt("shooter", NetFieldType::U8, 1, kMaxPlayers, NetFieldRole::Counter),
        netInt("victim", NetFieldType::U8, 1, kMaxPlayers, NetFieldRole::Counter)};
    std::vector<std::string> problems;
    if (!registry.addEvent(std::move(score), why) || !registry.freeze(problems)) {
        report.errors.push_back("the match registry didn't freeze: " + why);
        return;
    }
    JobSystem jobs(1);
    Harness h(options, faults);
    auto spawnCharacter = [](World& world, const NetworkPrefab& prefab, const Transform& at) {
        const Entity e = world.create();
        world.add<Transform>(e, at);
        world.add<Name>(e, {"character"});
        world.add<CharacterController>(e, {});
        NetworkObject n;
        n.profile = prefab.profile;
        n.mode = NetworkObject::Mode::PredictedCharacter;
        world.add<NetworkObject>(e, n);
        return e;
    };
    auto makeScene = [](World& world, uint32_t) {
        const Entity e = world.create();
        world.add<Name>(e, {"floor"});
        Transform t;
        t.position = {0, -0.5f, 0};
        world.add<Transform>(e, t);
        Collider c;
        c.halfExtents = {60, 0.5f, 60};
        world.add<Collider>(e, c);
        RigidBody body;
        body.motion = RigidBody::Motion::Static;
        world.add<RigidBody>(e, body);
    };
    h.replicate(registry, {{1, "Prefabs/Net Character.swprefab", "engine.pose.v1"}}, spawnCharacter, makeScene);
    const uint32_t rate = std::max<uint32_t>(options.settings.tickRate, 1);
    const uint32_t players = options.clients + 1;
    NetTestMatch& m = report.match;
    m.players = players;

    // Host state: each player's character by slot, and the match's switches.
    std::vector<Entity> pawns(kMaxPlayers + 1, kNullEntity);
    bool shooting = false, quiet = false;
    uint64_t physicsNanos = 0;   // the host's, this tick
    auto setup = [&](Node& node) {
        node.physics = std::make_unique<PhysicsWorld>(*node.scene, jobs);
        node.world->setPhysics(node.physics.get());
        if (node.world->role() == NetworkRole::Client) {
            node.world->setPredictionMove([](const World&, Entity, const InputCommand& c) { return scriptedMove(c); });
        } else {
            node.world->setCombatTargets([](const World& world, Entity e, CombatTarget& out) {
                const auto* id = world.get<NetworkIdentity>(e);
                if (!id || id->ownerSlot == 0) return false;
                out.team = uint8_t(id->ownerSlot % 2);
                out.eyeHeight = kEyeHeight;
                return true;
            });
        }
        return h.begin(node, report.errors);
    };
    // A map change or the first start: a fresh scene, physics and session.
    auto reload = [&](Node& node, const NetworkStart& start) {
        if (node.world) node.world->close();
        node.world.reset();
        node.physics.reset();
        node.scene = std::make_unique<World>();
        makeScene(*node.scene, 0);
        node.world = std::make_unique<NetworkWorld>(start, node.services.get(), node.scene.get());
        node.joinMs = -1;
        return setup(node);
    };

    Node& host = h.add("host", h.start(NetworkRole::Authority), kHostIdentity);
    if (!setup(host)) return h.finish(report);
    const uint32_t late = options.clients >= 2 ? options.clients : 0;   // the late joiner, 1-based
    for (uint32_t i = 1; i <= options.clients; ++i) {
        Node& client = h.add("client " + std::to_string(i), h.start(NetworkRole::Client), kHostIdentity + i);
        if (i != late)
            setup(client);
        else
            client.world.reset();   // it starts 3 s later
    }
    auto hostSpawnOwn = [&] {
        NetworkWorld& net = *h.nodes()[0].world;
        Transform at;
        at.position = spawnFor(1);
        if (auto own = net.spawn(1, at, 1)) pawns[1] = own.value();
    };
    hostSpawnOwn();

    // Score events each client read, while counting.
    std::vector<uint64_t> cursors(h.nodes().size(), 0), scores(h.nodes().size(), 0);
    bool counting = false;
    h.onHostTick = [&](uint32_t tick) {
        Node& hn = h.nodes()[0];
        NetworkWorld& net = *hn.world;
        World& world = *hn.scene;
        NetworkEvent event;
        while (net.pollEvent(event)) {
            if (event.kind == NetworkEventKind::PlayerJoined && event.slot <= kMaxPlayers) {
                Transform at;
                at.position = spawnFor(event.slot);
                if (auto pawn = net.spawn(1, at, event.slot))
                    pawns[event.slot] = pawn.value();
                else
                    report.errors.push_back(std::string("spawn failed: ") + pawn.error().message);
            } else if (event.kind == NetworkEventKind::PlayerLeft && event.slot <= kMaxPlayers) {
                if (world.alive(pawns[event.slot])) (void)net.despawn(pawns[event.slot]);
                pawns[event.slot] = kNullEntity;
            }
        }
        for (uint16_t slot = 1; slot <= players; ++slot) {
            const Entity pawn = pawns[slot];
            if (!world.alive(pawn)) continue;
            InputCommand c;
            if (slot == 1)
                c = scripted(0, tick, rate, quiet);
            else
                net.takeInput(slot, net.identity(pawn) ? net.identity(pawn)->incarnation : 0, c);
            const PredictedMove move = scriptedMove(c);
            CharacterController& controller = *world.get<CharacterController>(pawn);
            controller.desiredVelocity = move.desiredVelocity;
            controller.jumpRequested = controller.jumpRequested || move.jump;
            if (slot == 1 || !shooting || !(c.heldBits & InputCommand::kHeldFire)) continue;
            ShotQuery shot;
            shot.shooter = pawn;
            shot.origin = world.get<Transform>(pawn)->position + glm::vec3(0, kEyeHeight, 0);
            shot.direction = aimDirection(c);
            shot.range = 100;
            shot.command = &c;
            ShotResult result;
            if (!net.resolveShot(shot, result)) continue;
            ++m.shots;
            m.refusedShots += result.rewind == ShotRewind::Refused;
            if (!result.player) continue;
            const NetworkIdentity* victim = net.identity(result.hit);
            if (!victim || victim->ownerSlot == 0) continue;
            ++m.hits;
            const uint16_t victimSlot = victim->ownerSlot;
            Transform at;
            at.position = spawnFor(victimSlot);
            if (net.respawn(result.hit, &at)) ++m.respawns;
            ProfileValue payload;
            payload.fields[0].u = slot;
            payload.fields[1].u = victimSlot;
            if (auto sent = net.sendEvent(kScoreEvent, result.hit, payload); !sent)
                report.errors.push_back(std::string("a score event was refused: ") + sent.error().message);
        }
    };
    h.onTick = [&](Node& node, uint32_t tick) {
        NetworkWorld& net = *node.world;
        uint32_t index = 0;
        for (uint32_t i = 0; i < h.nodes().size(); ++i)
            if (&h.nodes()[i] == &node) index = i;
        if (net.role() == NetworkRole::Client && net.state() == NetworkState::Active) {
            const Entity mine = net.controlledEntity();
            const NetworkIdentity* identity = net.identity(mine);
            InputCommand c = scripted(index, tick, rate, quiet);
            c.incarnation = identity ? identity->incarnation : 0;
            const uint16_t slot = net.localSlot();
            // Once a second, aim at the next player as this machine shows them, and fire.
            if (shooting && !quiet && identity && (tick + slot * 13u) % rate == 0) {
                const uint16_t target = uint16_t(slot % players + 1);
                Entity aimed = kNullEntity;
                node.scene->each<const NetworkIdentity>([&](Entity e, const NetworkIdentity& id) {
                    if (id.ownerSlot == target) aimed = e;
                });
                const Transform* own = node.scene->get<Transform>(mine);
                if (own && node.scene->alive(aimed)) {
                    const PresentationTransform* shown = node.scene->get<PresentationTransform>(aimed);
                    const glm::vec3 there = shown ? shown->value.position : node.scene->get<Transform>(aimed)->position;
                    aimAt(c, own->position + glm::vec3(0, kEyeHeight, 0), there + glm::vec3(0, kChestHeight, 0));
                    c.heldBits |= InputCommand::kHeldFire;
                }
            }
            if (identity) net.sendInput(c);
        }
        net.prepareStep();
        const auto began = std::chrono::steady_clock::now();
        node.physics->step(1.0 / double(rate));
        if (net.role() == NetworkRole::Authority)
            physicsNanos = uint64_t(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count());
        if (net.role() == NetworkRole::Client) {
            net.present(1.0f);
            if (counting) {
                ReceivedGameEvent e;
                while (net.readGameEvent(cursors[index], e)) scores[index] += e.eventId == kScoreEvent;
            }
        }
    };

    // ---- Joining, one player late ----
    auto everyoneActive = [&](uint32_t epoch) {
        for (Node& n : h.nodes())
            if (!n.world || n.world->state() != NetworkState::Active || n.world->mapEpoch() != epoch) return false;
        return h.nodes()[0].world->stats().activePeers == options.clients;
    };
    if (late) {
        h.runUntil([] { return false; }, h.ticksFor(3) - 1);
        const double lateBegan = h.nowSeconds() * 1000;
        Node& lateNode = h.nodes()[late];
        lateNode.world =
            std::make_unique<NetworkWorld>(h.start(NetworkRole::Client), lateNode.services.get(), lateNode.scene.get());
        setup(lateNode);
        if (!h.runUntil([&] { return everyoneActive(1); }, h.ticksFor(options.settings.handshakeTimeoutSeconds + 5))) {
            report.errors.push_back("not every player joined");
            return h.stopAll(report);
        }
        m.lateJoinMs = h.nodes()[late].joinMs - lateBegan;
    } else if (!h.runUntil(
                   [&] { return everyoneActive(1); }, h.ticksFor(options.settings.handshakeTimeoutSeconds + 5))) {
        report.errors.push_back("not every player joined");
        return h.stopAll(report);
    }
    std::vector<uint16_t> slots;
    for (Node& n : h.nodes()) slots.push_back(n.world->localSlot());

    // ---- 10 s of play, measured ----
    std::vector<NetworkPeerInfo> info;
    struct Totals {
        uint64_t up = 0, down = 0;
    };
    auto totals = [&](Node& n) {
        Totals t;
        n.world->peerInfo(info);
        for (const NetworkPeerInfo& p : info) t.up += p.bytesSent, t.down += p.bytesReceived;
        return t;
    };
    std::vector<Totals> before;
    for (Node& n : h.nodes()) before.push_back(totals(n));
    for (size_t i = 1; i < h.nodes().size(); ++i) cursors[i] = h.nodes()[i].world->gameEventCursor();
    const NetworkWorldStats startStats = h.nodes()[0].world->stats();
    shooting = counting = true;
    const uint32_t measured = 10 * rate;
    std::vector<double> hostTicks;
    double physicsTotal = 0, clientTotal = 0;
    uint64_t clientSteps = 0;
    for (uint32_t t = 0; t < measured; ++t) {
        h.step();
        hostTicks.push_back(double(h.nodes()[0].stepNanos) / 1e6);
        physicsTotal += double(physicsNanos) / 1e6;
        for (size_t i = 1; i < h.nodes().size(); ++i) {
            const double ms = double(h.nodes()[i].stepNanos) / 1e6;
            clientTotal += ms;
            ++clientSteps;
            m.clientTickMaxMs = std::max(m.clientTickMaxMs, ms);
        }
        if (t % (rate / 2) == 0) {
            for (size_t i = 0; i < h.nodes().size(); ++i) {
                NetworkWorld& w = *h.nodes()[i].world;
                (i == 0 ? m.hostMemoryBytes : m.clientMemoryBytes) =
                    std::max<uint64_t>(i == 0 ? m.hostMemoryBytes : m.clientMemoryBytes, w.memoryBytes());
            }
            h.nodes()[0].world->peerInfo(info);
            for (const NetworkPeerInfo& p : info) {
                m.pendingReliableHigh = std::max(m.pendingReliableHigh, p.pendingReliableBytes);
                m.pendingUnreliableHigh = std::max(m.pendingUnreliableHigh, p.pendingUnreliableBytes);
            }
            m.degradedPeersHigh = std::max(m.degradedPeersHigh, h.nodes()[0].world->stats().degradedPeers);
        }
    }
    shooting = false;
    m.measuredSeconds = double(measured) / rate;
    const NetworkWorldStats& hs = h.nodes()[0].world->stats();
    m.hostTickMs = hostTicks.empty() ? 0 : std::accumulate(hostTicks.begin(), hostTicks.end(), 0.0) / hostTicks.size();
    m.hostTickP99Ms = percentile(hostTicks, 0.99);
    m.hostTickMaxMs = percentile(hostTicks, 1.0);
    m.hostPhysicsMs = physicsTotal / measured;
    m.hostNetworkMs = double(hs.workNanos - startStats.workNanos) / 1e6 / measured;
    m.clientTickMs = clientSteps ? clientTotal / double(clientSteps) : 0;
    const Totals hostNow = totals(h.nodes()[0]);
    m.hostUpBytesPerSecond = double(hostNow.up - before[0].up) / m.measuredSeconds;
    m.hostDownBytesPerSecond = double(hostNow.down - before[0].down) / m.measuredSeconds;
    m.hostUpPerClient = m.hostUpBytesPerSecond / options.clients;
    m.hostMessagesPerSecond = double(hs.messagesSent - startStats.messagesSent) / m.measuredSeconds;
    for (size_t i = 1; i < h.nodes().size(); ++i) {
        const Totals now = totals(h.nodes()[i]);
        m.clientDownBytesPerSecond += double(now.down - before[i].down) / m.measuredSeconds / options.clients;
        m.clientUpBytesPerSecond += double(now.up - before[i].up) / m.measuredSeconds / options.clients;
    }
    m.controlQueueHigh = hs.controlQueueHigh;
    m.recordsLimited = hs.recordsLimited - startStats.recordsLimited;
    m.updateLevelRaises = hs.updateLevelRaises - startStats.updateLevelRaises;
    m.updateLevelDrops = hs.updateLevelDrops - startStats.updateLevelDrops;
    // Every score event reaches every client (reliably, in order); how long the last
    // one took is reported.
    m.scoreEvents = hs.gameEventsSent - startStats.gameEventsSent;
    const double settleBegan = h.nowSeconds();
    h.runUntil(
        [&] {
            for (size_t i = 1; i < h.nodes().size(); ++i)
                if (scores[i] < m.scoreEvents) return false;
            return true;
        },
        5 * rate);
    m.eventsSettledMs = (h.nowSeconds() - settleBegan) * 1000;
    counting = false;
    for (size_t i = 1; i < h.nodes().size(); ++i)
        if (scores[i] != m.scoreEvents)
            report.errors.push_back(
                h.nodes()[i].name + " read " + std::to_string(scores[i]) + " of " + std::to_string(m.scoreEvents) +
                " score events within 5 s");
    if (m.hostUpPerClient > 1.1 * double(options.settings.outboundBytesPerSecond))
        report.errors.push_back(
            "the host sent " + std::to_string(int64_t(m.hostUpPerClient)) +
            " bytes/s per player, over outbound_bytes_per_second " +
            std::to_string(options.settings.outboundBytesPerSecond));

    // ---- The map changes ----
    MapRotationRequest request;
    request.level = "map-2";
    request.sceneId = 2;
    request.readySeconds = 10;
    const double rotationBegan = h.nowSeconds();
    if (auto prepared = h.nodes()[0].world->prepareRotation(request); !prepared) {
        report.errors.push_back(std::string("the host couldn't announce a map: ") + prepared.error().message);
        return h.stopAll(report);
    }
    DeterministicRandom random(options.seed ^ 0x3a7c4);
    std::vector<uint32_t> loadTicks;
    for (size_t i = 0; i < h.nodes().size(); ++i) loadTicks.push_back(1 + uint32_t(random.next() % h.ticksFor(0.5)));
    const uint32_t from = h.ticks();
    const bool committed = h.runUntil(
        [&] {
            bool all = true;
            for (size_t i = 0; i < h.nodes().size(); ++i) {
                NetworkWorld& w = *h.nodes()[i].world;
                if (w.rotation().phase == RotationPhase::Preparing && h.ticks() - from >= loadTicks[i])
                    w.rotationReady();
                all = all && w.rotation().phase == RotationPhase::Committed;
            }
            return all;
        },
        h.ticksFor(request.readySeconds + 2));
    if (!committed) {
        report.errors.push_back("the map change wasn't committed by everyone");
        return h.stopAll(report);
    }
    std::vector<NetworkStart> next;
    for (Node& n : h.nodes()) next.push_back(n.world->nextStart());
    std::fill(pawns.begin(), pawns.end(), kNullEntity);
    for (size_t i = 0; i < h.nodes().size(); ++i) reload(h.nodes()[i], next[i]);
    hostSpawnOwn();
    if (!h.runUntil([&] { return everyoneActive(2); }, h.ticksFor(15))) {
        report.errors.push_back("not everyone rejoined after the map change");
        return h.stopAll(report);
    }
    m.rotationMs = (h.nowSeconds() - rotationBegan) * 1000;
    for (size_t i = 0; i < h.nodes().size(); ++i)
        if (h.nodes()[i].world->localSlot() != slots[i])
            report.errors.push_back(h.nodes()[i].name + " came back in another slot");

    // ---- 4 s more, then everyone stands still ----
    shooting = true;
    h.runUntil([] { return false; }, 4 * rate);
    shooting = false;
    quiet = true;
    h.runUntil([] { return false; }, 3 * rate);
    Node& hn = h.nodes()[0];
    for (size_t i = 1; i < h.nodes().size(); ++i) {
        Node& n = h.nodes()[i];
        if (n.world->state() != NetworkState::Active) {
            report.errors.push_back(n.name + " isn't active (" + networkStateName(n.world->state()) + ")");
            continue;
        }
        const Entity mine = n.world->controlledEntity();
        const Transform* a = n.scene->get<Transform>(mine);
        const Transform* b = hn.scene->get<Transform>(hn.world->entity(n.world->networkId(mine)));
        if (!a || !b) {
            report.errors.push_back(n.name + "'s character is missing on one side");
            continue;
        }
        m.finalError = std::max(m.finalError, double(glm::length(a->position - b->position)));
        for (uint16_t slot = 1; slot <= players; ++slot) {
            const Entity pawn = pawns[slot];
            if (!hn.scene->alive(pawn)) continue;
            const Entity e = n.world->entity(hn.world->networkId(pawn));
            const PresentationTransform* shown = n.scene->get<PresentationTransform>(e);
            if (!shown) {
                report.errors.push_back(n.name + " doesn't show player " + std::to_string(slot));
                continue;
            }
            m.finalPresentationError = std::max(
                m.finalPresentationError,
                double(glm::length(shown->value.position - hn.scene->get<Transform>(pawn)->position)));
        }
    }
    if (m.finalError > NetworkWorld::kPositionTolerance + 0.002)
        report.errors.push_back("a client's character isn't where the host has it after standing still");
    if (m.finalPresentationError > 0.01)
        report.errors.push_back("a client draws a character away from where the host has it after standing still");
    if (m.shots && !m.hits) report.warnings.push_back("no shot hit anyone");
    if (m.shots && m.refusedShots * 4 > m.shots) report.warnings.push_back("over a quarter of the shots were refused");
    const NetTestMatch kept = m;
    h.stopAll(report);
    report.match = kept;
    checkClean(report);
}

}   // namespace

std::span<const char* const> netTestScenarios() { return kScenarios; }

bool runNetTest(const NetTestOptions& options, NetTestReport& report) {
    report = {};
    report.scenario = options.scenario;
    report.seed = options.seed;
    report.preset = options.preset;
    const FaultPreset* preset = findFaultPreset(options.preset);
    if (!preset) {
        report.errors.push_back("unknown fault preset " + options.preset + " (clean, cafe, poor or hostile)");
        return false;
    }
    if (options.clients < 1 ||
        options.clients + 1 > std::clamp<uint32_t>(options.settings.maxPlayers, 2, kMaxPlayers)) {
        report.errors.push_back(
            "--clients must be 1 to max_players - 1 (" + std::to_string(options.settings.maxPlayers - 1) + ")");
        return false;
    }
    if (options.scenario == "handshake")
        handshake(options, preset->profile, report);
    else if (options.scenario == "rejections")
        rejections(options, report);
    else if (options.scenario == "stop-start")
        stopStart(options, report);
    else if (options.scenario == "replication")
        replication(options, preset->profile, report);
    else if (options.scenario == "prediction")
        prediction(options, preset->profile, report);
    else if (options.scenario == "map_rotation")
        mapRotation(options, preset->profile, report);
    else if (options.scenario == "match")
        match(options, preset->profile, report);
    else {
        report.errors.push_back(
            "unknown scenario " + options.scenario +
            " (handshake, rejections, stop-start, replication, prediction, map_rotation or match)");
        return false;
    }
    report.ok = report.errors.empty();
    return true;
}

std::string netTestText(const NetTestReport& r) {
    std::string out;
    char line[256];
    std::snprintf(
        line, sizeof(line), "net-test %s seed %llu preset %s: %s\n", r.scenario.c_str(), (unsigned long long)r.seed,
        r.preset.c_str(), r.ok ? "ok" : "FAILED");
    out += line;
    for (const NetTestPeer& p : r.peers) {
        std::snprintf(
            line, sizeof(line), "peer %s: %s, %s%s%s, slot %u", p.name.c_str(), networkRoleName(p.role),
            networkStateName(p.state), p.state == NetworkState::Failed ? " " : "",
            p.state == NetworkState::Failed ? reasonName(p.reason) : "", unsigned(p.slot));
        out += line;
        if (p.joinMs >= 0) {
            std::snprintf(line, sizeof(line), ", joined at %.0f ms", p.joinMs);
            out += line;
        }
        out += "\n";
    }
    if (r.rounds) out += "rounds " + std::to_string(r.rounds) + "\n";
    if (r.scenario == "map_rotation") {
        std::snprintf(
            line, sizeof(line),
            "rotations %u, slots reclaimed %llu, rejoin attempts %llu, longest %.0f ms, mean %.0f ms\n",
            r.rotation.rotations, (unsigned long long)r.rotation.slotsReclaimed,
            (unsigned long long)r.rotation.rejoinAttempts, r.rotation.maxRotationMs, r.rotation.meanRotationMs);
        out += line;
    }
    if (r.scenario == "match") {
        const NetTestMatch& m = r.match;
        out += std::format(
            "match: {} players over {:.0f} s; late join {:.0f} ms, map change {:.0f} ms\n"
            "  host tick {:.3f} ms (p99 {:.3f}, max {:.3f}): physics {:.3f}, network {:.3f}; client tick {:.3f} ms "
            "(max {:.3f})\n"
            "  host up {:.0f} B/s ({:.0f} per player, {:.0f} messages/s), down {:.0f} B/s; client down {:.0f} B/s, up "
            "{:.0f} B/s\n"
            "  queues: control {} B, transport reliable {} B, unreliable {} B; memory host {} KiB, client {} KiB\n"
            "  budget: {} records held back, {} slow-downs, {} speed-ups, at most {} players degraded\n"
            "  shots {}, hits {}, refused {}, respawns {}, score events {} (all in {:.0f} ms); final error {:.4f} m "
            "(drawn {:.4f} m)\n",
            m.players, m.measuredSeconds, m.lateJoinMs, m.rotationMs, m.hostTickMs, m.hostTickP99Ms, m.hostTickMaxMs,
            m.hostPhysicsMs, m.hostNetworkMs, m.clientTickMs, m.clientTickMaxMs, m.hostUpBytesPerSecond,
            m.hostUpPerClient, m.hostMessagesPerSecond, m.hostDownBytesPerSecond, m.clientDownBytesPerSecond,
            m.clientUpBytesPerSecond, m.controlQueueHigh, m.pendingReliableHigh, m.pendingUnreliableHigh,
            m.hostMemoryBytes / 1024, m.clientMemoryBytes / 1024, m.recordsLimited, m.updateLevelRaises,
            m.updateLevelDrops, m.degradedPeersHigh, m.shots, m.hits, m.refusedShots, m.respawns, m.scoreEvents,
            m.eventsSettledMs, m.finalError, m.finalPresentationError);
    }
    std::snprintf(
        line, sizeof(line),
        "ticks %u, messages %llu sent, %llu received, %llu rejected, %llu malformed; after stop: %llu packets in "
        "flight, %llu auth sessions, %llu tickets\n",
        r.durationTicks, (unsigned long long)r.messagesSent, (unsigned long long)r.messagesReceived,
        (unsigned long long)r.rejected, (unsigned long long)r.malformed, (unsigned long long)r.packetsInFlightAfterStop,
        (unsigned long long)r.openAuthSessionsAfterStop, (unsigned long long)r.liveTicketsAfterStop);
    out += line;
    const NetTestReplication& rep = r.replication;
    if (rep.objects) {
        std::snprintf(
            line, sizeof(line),
            "replication: %llu objects, %llu spawns, %llu despawns, %llu snapshots (%llu parts), %llu records "
            "applied, %llu stale, %llu held, %llu expired, %llu resyncs, control queue high %u bytes, max position "
            "error %.6f m\n",
            (unsigned long long)rep.objects, (unsigned long long)rep.spawns, (unsigned long long)rep.despawns,
            (unsigned long long)rep.snapshotsSent, (unsigned long long)rep.snapshotPartsSent,
            (unsigned long long)rep.recordsApplied, (unsigned long long)rep.staleRecords,
            (unsigned long long)rep.heldSnapshots, (unsigned long long)rep.expiredSnapshots,
            (unsigned long long)rep.resyncRequests, unsigned(rep.controlQueueHigh), rep.maxPositionError);
        out += line;
    }
    const NetTestPrediction& pr = r.prediction;
    if (pr.predictedTicks) {
        std::snprintf(
            line, sizeof(line),
            "prediction: %llu ticks predicted, %llu checked, %llu matched, %llu corrections (%llu ticks replayed, max "
            "%.3f m), %llu resets, %llu catch-up inputs, %llu host fallbacks, final error %.4f m (drawn %.4f m), "
            "largest offset shown %.3f m, host trigger events %llu\n",
            (unsigned long long)pr.predictedTicks, (unsigned long long)pr.checks, (unsigned long long)pr.matches,
            (unsigned long long)pr.corrections, (unsigned long long)pr.replayedTicks, pr.maxCorrection,
            (unsigned long long)pr.resets, (unsigned long long)pr.catchUpInputs, (unsigned long long)pr.hostFallbacks,
            pr.finalError, pr.finalPresentationError, pr.maxOffset, (unsigned long long)pr.hostContacts);
        out += line;
    }
    for (const std::string& e : r.errors) out += "error " + e + "\n";
    for (const std::string& w : r.warnings) out += "warning " + w + "\n";
    return out;
}

std::string netTestJson(const NetTestReport& r) {
    auto list = [](const std::vector<std::string>& items) {
        std::string out;
        for (const std::string& item : items) out += std::string(out.empty() ? "" : ", ") + jsonQuote(item);
        return "[" + out + "]";
    };
    auto reason = [](ReasonCode code) { return jsonQuote(reasonName(code) ? reasonName(code) : "unknown"); };
    std::string out = "{\n  \"version\": 1,\n  \"mode\": \"net-test\",\n";
    out += "  \"scenario\": " + jsonQuote(r.scenario) + ",\n";
    out += "  \"ok\": " + std::string(r.ok ? "true" : "false") + ",\n";
    out += "  \"errors\": " + list(r.errors) + ",\n  \"warnings\": " + list(r.warnings) + ",\n";
    out += "  \"seed\": \"" + std::to_string(r.seed) + "\",\n";
    out += "  \"preset\": " + jsonQuote(r.preset) + ",\n";
    out += "  \"duration_ticks\": " + std::to_string(r.durationTicks) + ",\n";
    out += "  \"peers\": [";
    for (size_t i = 0; i < r.peers.size(); ++i) {
        const NetTestPeer& p = r.peers[i];
        out += std::string(i ? "," : "") + "\n    {\"name\": " + jsonQuote(p.name) +
               ", \"role\": " + jsonQuote(networkRoleName(p.role)) +
               ", \"state\": " + jsonQuote(networkStateName(p.state)) + ", \"reason\": " + reason(p.reason) +
               ", \"expected\": " + reason(p.expected) + ", \"slot\": " + std::to_string(p.slot) + ", \"peer\": \"" +
               std::to_string(p.peer) +
               "\", \"join_ms\": " + (p.joinMs >= 0 ? std::to_string(int64_t(p.joinMs)) : std::string("null")) + "}";
    }
    out += r.peers.empty() ? "],\n" : "\n  ],\n";
    if (r.scenario == "map_rotation")
        out += "  \"rotation\": {\"rotations\": " + std::to_string(r.rotation.rotations) +
               ", \"slots_reclaimed\": " + std::to_string(r.rotation.slotsReclaimed) +
               ", \"rejoin_attempts\": " + std::to_string(r.rotation.rejoinAttempts) +
               ", \"max_ms\": " + std::to_string(int64_t(r.rotation.maxRotationMs)) +
               ", \"mean_ms\": " + std::to_string(int64_t(r.rotation.meanRotationMs)) + "},\n";
    if (r.scenario == "match") {
        const NetTestMatch& m = r.match;
        out += std::format(
            "  \"match\": {{\"players\": {}, \"measured_s\": {:.1f}, \"late_join_ms\": {:.0f}, \"rotation_ms\": "
            "{:.0f}, "
            "\"host_tick_ms\": {:.4f}, \"host_tick_p99_ms\": {:.4f}, \"host_tick_max_ms\": {:.4f}, "
            "\"host_physics_ms\": {:.4f}, \"host_network_ms\": {:.4f}, \"client_tick_ms\": {:.4f}, "
            "\"client_tick_max_ms\": {:.4f}, \"host_up_bytes_per_s\": {:.0f}, \"host_up_per_player\": {:.0f}, "
            "\"host_down_bytes_per_s\": {:.0f}, \"host_messages_per_s\": {:.0f}, \"client_down_bytes_per_s\": {:.0f}, "
            "\"client_up_bytes_per_s\": {:.0f}, \"control_queue_high\": {}, \"pending_reliable_high\": {}, "
            "\"pending_unreliable_high\": {}, \"host_memory_bytes\": {}, \"client_memory_bytes\": {}, "
            "\"records_limited\": {}, \"update_level_raises\": {}, \"update_level_drops\": {}, "
            "\"degraded_peers_high\": {}, \"shots\": {}, "
            "\"hits\": {}, \"refused_shots\": {}, \"respawns\": {}, \"score_events\": {}, \"events_settled_ms\": "
            "{:.0f}, \"final_error_m\": {:.6f}, "
            "\"final_presentation_error_m\": {:.6f}}},\n",
            m.players, m.measuredSeconds, m.lateJoinMs, m.rotationMs, m.hostTickMs, m.hostTickP99Ms, m.hostTickMaxMs,
            m.hostPhysicsMs, m.hostNetworkMs, m.clientTickMs, m.clientTickMaxMs, m.hostUpBytesPerSecond,
            m.hostUpPerClient, m.hostDownBytesPerSecond, m.hostMessagesPerSecond, m.clientDownBytesPerSecond,
            m.clientUpBytesPerSecond, m.controlQueueHigh, m.pendingReliableHigh, m.pendingUnreliableHigh,
            m.hostMemoryBytes, m.clientMemoryBytes, m.recordsLimited, m.updateLevelRaises, m.updateLevelDrops,
            m.degradedPeersHigh, m.shots, m.hits, m.refusedShots, m.respawns, m.scoreEvents, m.eventsSettledMs,
            m.finalError, m.finalPresentationError);
    }
    out += "  \"counters\": {\"rounds\": " + std::to_string(r.rounds) +
           ", \"messages_sent\": " + std::to_string(r.messagesSent) +
           ", \"messages_received\": " + std::to_string(r.messagesReceived) +
           ", \"rejected\": " + std::to_string(r.rejected) + ", \"malformed\": " + std::to_string(r.malformed) +
           ", \"packets_in_flight_after_stop\": " + std::to_string(r.packetsInFlightAfterStop) +
           ", \"open_auth_sessions_after_stop\": " + std::to_string(r.openAuthSessionsAfterStop) +
           ", \"live_tickets_after_stop\": " + std::to_string(r.liveTicketsAfterStop) + "},\n";
    const NetTestReplication& rep = r.replication;
    char error[64];
    std::snprintf(error, sizeof(error), "%.6f", rep.maxPositionError);
    out += "  \"replication\": {\"objects\": " + std::to_string(rep.objects) +
           ", \"spawns\": " + std::to_string(rep.spawns) + ", \"despawns\": " + std::to_string(rep.despawns) +
           ", \"snapshots_sent\": " + std::to_string(rep.snapshotsSent) +
           ", \"snapshot_parts_sent\": " + std::to_string(rep.snapshotPartsSent) +
           ", \"records_applied\": " + std::to_string(rep.recordsApplied) +
           ", \"stale_records\": " + std::to_string(rep.staleRecords) +
           ", \"held_snapshots\": " + std::to_string(rep.heldSnapshots) +
           ", \"expired_snapshots\": " + std::to_string(rep.expiredSnapshots) +
           ", \"resync_requests\": " + std::to_string(rep.resyncRequests) +
           ", \"control_queue_high\": " + std::to_string(rep.controlQueueHigh) +
           ", \"max_position_error_m\": " + error + "},\n";
    const NetTestPrediction& pr = r.prediction;
    char metres[192];
    std::snprintf(
        metres, sizeof(metres),
        "%.6f, \"final_error_m\": %.6f, \"final_presentation_error_m\": %.6f, \"max_offset_m\": %.6f", pr.maxCorrection,
        pr.finalError, pr.finalPresentationError, pr.maxOffset);
    out += "  \"prediction\": {\"predicted_ticks\": " + std::to_string(pr.predictedTicks) +
           ", \"checks\": " + std::to_string(pr.checks) + ", \"matches\": " + std::to_string(pr.matches) +
           ", \"corrections\": " + std::to_string(pr.corrections) +
           ", \"replayed_ticks\": " + std::to_string(pr.replayedTicks) + ", \"resets\": " + std::to_string(pr.resets) +
           ", \"catch_up_inputs\": " + std::to_string(pr.catchUpInputs) +
           ", \"host_fallbacks\": " + std::to_string(pr.hostFallbacks) +
           ", \"host_contacts\": " + std::to_string(pr.hostContacts) + ", \"max_correction_m\": " + metres + "}\n}\n";
    return out;
}

}   // namespace shockwave::net
