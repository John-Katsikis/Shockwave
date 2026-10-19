// player --net-test scenarios: NetworkWorlds on a LoopbackHub (Multiplayer/NetTest.h).
#include <Multiplayer/NetTest.h>

#include <Multiplayer/NetworkSchema.h>
#include <Multiplayer/Replication.h>

#include <Engine/Components.h>
#include <Network/LoopbackServices.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>

namespace shockwave::net {

namespace {

constexpr const char* kScenarios[] = {"handshake", "rejections", "stop-start", "replication"};
constexpr uint64_t kHostIdentity = 1000;

struct Node {
    std::string name;
    std::unique_ptr<LoopbackOnlineServices> services;
    std::unique_ptr<World> scene;   // replication: this machine's world
    std::unique_ptr<NetworkWorld> world;
    ReasonCode expected = ReasonCode::Normal;
    double joinMs = -1;
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
            node.services->pump();
            OnlineEvent event;
            while (node.services->pollEvent(event)) node.world->handleOnlineEvent(event);
            node.world->pump(now);
            node.world->drain(m_ticks);
            if (node.world->role() == NetworkRole::Authority && onHostTick) onHostTick(m_ticks);
            node.world->endTick(m_ticks);
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
    else {
        report.errors.push_back(
            "unknown scenario " + options.scenario + " (handshake, rejections, stop-start or replication)");
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
           ", \"max_position_error_m\": " + error + "}\n}\n";
    return out;
}

}   // namespace shockwave::net
