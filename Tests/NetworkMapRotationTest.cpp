// Map rotation (shipping plan S11, docs/Networking.md "Map rotation"): NetworkWorlds over an
// in-process LoopbackHub change maps by controlled session restart. Covered: a player who
// never gets ready (dropped, or the rotation called off), a player who can't load the map,
// joins in every phase, reserved slots, host loss before and after the commit, a next
// session that isn't the one announced, duplicate and stale answers from a raw peer, and
// many rotations in a row (the --net-test map_rotation scenario, on every fault preset).
#include <Core/Jobs.h>
#include <Engine/Components.h>
#include <GameFramework/LevelHost.h>
#include <GameFramework/NetworkPump.h>
#include <Multiplayer/NetTest.h>
#include <Multiplayer/NetworkWorld.h>
#include <Network/LoopbackServices.h>
#include <Network/Messages.h>

#include <array>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace shockwave;
using namespace shockwave::net;

namespace {

int failures = 0, checks = 0;
void check(bool value, const std::string& message) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

constexpr uint64_t kHost = 1000;

struct Node {
    std::unique_ptr<LoopbackOnlineServices> services;
    std::unique_ptr<NetworkWorld> world;
};

// A host and clients on one virtual clock, stepped a 60 Hz tick at a time.
struct Net {
    std::unique_ptr<LoopbackHub> hub;
    std::deque<Node> nodes;
    std::vector<std::function<void()>> extra;   // raw peers, polled every step
    uint32_t tick = 0;
    bool authenticate = true;

    explicit Net(bool auth = true) : authenticate(auth) {
        LoopbackHubConfig config;
        config.seed = 3;
        hub = std::make_unique<LoopbackHub>(config);
    }
    NetworkStart start(NetworkRole role) const {
        NetworkStart s;
        s.role = role;
        s.settings.tickRate = 60;
        s.settings.maxPlayers = 8;
        s.compatibility.gameProtocol = "rotation-test";
        s.compatibility.schemaDigest[0] = 1;
        s.compatibility.contentDigest[0] = 2;
        s.address = {TransportAddress::Loopback, 9, 0};
        s.authenticate = authenticate;
        return s;
    }
    Node& add(NetworkRole role, uint64_t identity) {
        Node& n = nodes.emplace_back();
        n.services = std::make_unique<LoopbackOnlineServices>(*hub, PeerId{identity});
        begin(n, start(role));
        return n;
    }
    void begin(Node& n, const NetworkStart& s) {
        n.world = std::make_unique<NetworkWorld>(s, n.services.get(), nullptr);
        check(bool(n.world->begin()), "a session starts");
    }
    void restart(Node& n) {
        const NetworkStart next = n.world->nextStart();
        n.world->close();
        begin(n, next);
    }
    double now() const { return double(hub->nowUs()) / 1e6; }
    void step() {
        for (Node& n : nodes) {
            if (!n.world) continue;
            n.services->pump();
            OnlineEvent event;
            while (n.services->pollEvent(event)) n.world->handleOnlineEvent(event);
            n.world->pump(now());
            n.world->drain(tick);
            n.world->endTick(tick);
        }
        for (auto& f : extra) f();
        hub->advance(16667);
        ++tick;
    }
    bool until(const std::function<bool()>& done, int ticks) {
        for (int i = 0; i < ticks; ++i) {
            if (done()) return true;
            step();
        }
        return done();
    }
    void run(int ticks) {
        for (int i = 0; i < ticks; ++i) step();
    }
};

bool active(const Node& n) { return n.world && n.world->state() == NetworkState::Active; }
RotationPhase phase(const Node& n) { return n.world->rotation().phase; }

MapRotationRequest nextMap(double seconds = 2, bool drop = true) {
    MapRotationRequest r;
    r.level = "factory";
    r.sceneId = 2;
    r.readySeconds = seconds;
    r.dropUnready = drop;
    return r;
}

// Host plus `clients` clients, all Active.
void join(Net& net, int clients) {
    net.add(NetworkRole::Authority, kHost);
    for (int i = 0; i < clients; ++i) net.add(NetworkRole::Client, kHost + 1 + uint64_t(i));
    const bool joined = net.until(
        [&] {
            for (const Node& n : net.nodes)
                if (!active(n)) return false;
            return net.nodes[0].world->stats().activePeers == uint32_t(clients);
        },
        600);
    check(joined, "everyone joins");
}

// ---- A player who never gets ready -------------------------------------------------------

void neverReady() {
    Net net;
    join(net, 2);
    Node &host = net.nodes[0], &a = net.nodes[1], &b = net.nodes[2];
    const uint16_t slotA = a.world->localSlot();
    check(bool(host.world->prepareRotation(nextMap(2))), "the host announces the next map");
    check(!host.world->prepareRotation(nextMap(2)), "a second announcement is refused while one is under way");
    net.until([&] { return phase(a) == RotationPhase::Preparing && phase(b) == RotationPhase::Preparing; }, 60);
    check(a.world->rotation().level == "factory" && a.world->rotation().sceneId == 2, "clients learn the map");
    host.world->rotationReady();
    a.world->rotationReady();
    a.world->rotationReady();   // twice: sent once
    net.until([&] { return phase(host) != RotationPhase::Preparing; }, 60 * 4);
    check(phase(host) == RotationPhase::Committed, "at the deadline the host goes on without the late player");
    check(host.world->stats().rotationDrops == 1, "one player was dropped");
    check(host.world->rotation().duplicates == 0, "a repeated ready isn't sent twice");
    net.until([&] { return b.world->state() == NetworkState::Failed && phase(a) == RotationPhase::Committed; }, 60);
    check(b.world->failure() == ReasonCode::MapNotLoaded, "the late player is told the map didn't load in time");
    check(phase(a) == RotationPhase::Committed, "the ready player follows");
    const NetworkStart next = host.world->nextStart();
    check(next.mapEpoch == 2 && next.compatibility.sceneId == 2 && next.presetSession, "the next session is decided");
    check(
        next.reservedSlots.size() == 1 && next.reservedSlots[0].second == slotA,
        "only the ready player's slot is held");
    net.restart(host);
    net.restart(a);
    net.until([&] { return active(a); }, 600);
    check(active(a) && a.world->localSlot() == slotA && a.world->mapEpoch() == 2, "it rejoins in its own slot");
    check(host.world->stats().slotsReclaimed == 1, "the host gave the slot back");
}

// ---- Called off when a player isn't ready, and a later one works ------------------------------

void abortThenRetry() {
    Net net;
    join(net, 2);
    Node &host = net.nodes[0], &a = net.nodes[1], &b = net.nodes[2];
    check(bool(host.world->prepareRotation(nextMap(1.5, false))), "announced, without dropping anyone");
    host.world->rotationReady();
    net.until([&] { return phase(a) == RotationPhase::Preparing; }, 60);
    a.world->rotationReady();
    net.until([&] { return phase(host) != RotationPhase::Preparing; }, 60 * 4);
    check(phase(host) == RotationPhase::Aborted, "a player who isn't ready calls it off");
    check(host.world->rotation().abortReason == ReasonCode::MapNotLoaded, "because the map didn't load");
    net.until([&] { return phase(a) == RotationPhase::Aborted && phase(b) == RotationPhase::Aborted; }, 60);
    check(phase(a) == RotationPhase::Aborted && phase(b) == RotationPhase::Aborted, "clients hear it was called off");
    check(active(a) && active(b) && active(host), "and the current map plays on");
    // A later rotation, with everyone ready.
    check(bool(host.world->prepareRotation(nextMap(5))), "a later rotation can be announced");
    check(host.world->rotation().transitionId == 2, "with the next transition ID");
    host.world->rotationReady();
    net.until([&] { return phase(a) == RotationPhase::Preparing && phase(b) == RotationPhase::Preparing; }, 60);
    a.world->rotationReady();
    b.world->rotationReady();
    net.until([&] { return phase(a) == RotationPhase::Committed && phase(b) == RotationPhase::Committed; }, 120);
    check(phase(host) == RotationPhase::Committed && phase(b) == RotationPhase::Committed, "it's committed for all");
}

// ---- A player who can't load the map -----------------------------------------------------------

void clientRejects() {
    Net net;
    join(net, 2);
    Node &host = net.nodes[0], &a = net.nodes[1], &b = net.nodes[2];
    check(bool(host.world->prepareRotation(nextMap(5))), "the host announces the next map");
    host.world->rotationReady();
    net.until([&] { return phase(a) == RotationPhase::Preparing && phase(b) == RotationPhase::Preparing; }, 60);
    b.world->rejectRotation(ReasonCode::ContentMismatch);
    check(phase(b) == RotationPhase::Aborted, "a client that can't load the map stops preparing");
    a.world->rotationReady();
    net.until([&] { return phase(host) == RotationPhase::Committed; }, 120);
    check(phase(host) == RotationPhase::Committed, "the host goes on with the others at once");
    net.until([&] { return b.world->state() == NetworkState::Failed; }, 60);
    check(b.world->failure() == ReasonCode::ContentMismatch, "the client is told why it stays behind");
}

// ---- Joining in every phase, and reserved slots ------------------------------------------------

void lateJoins() {
    Net net;
    join(net, 2);
    Node &host = net.nodes[0], &a = net.nodes[1], &b = net.nodes[2];
    const uint16_t slotA = a.world->localSlot(), slotB = b.world->localSlot();
    // Still in the handshake when the map changes: refused, to join the next map.
    Node& early = net.add(NetworkRole::Client, kHost + 10);
    net.run(2);
    check(!active(early), "a join is under way");
    check(bool(host.world->prepareRotation(nextMap(5))), "the host announces the next map");
    net.until([&] { return early.world->state() == NetworkState::Failed; }, 120);
    check(early.world->failure() == ReasonCode::MapRotation, "a join under way is refused while the map changes");
    // Arriving while it's being prepared: refused too.
    Node& during = net.add(NetworkRole::Client, kHost + 11);
    net.until([&] { return during.world->state() == NetworkState::Failed; }, 120);
    check(during.world->failure() == ReasonCode::MapRotation, "a join during the preparation is refused");
    check(host.world->stats().rotationRefusals >= 2, "and counted");
    host.world->rotationReady();
    net.until([&] { return phase(a) == RotationPhase::Preparing && phase(b) == RotationPhase::Preparing; }, 60);
    a.world->rotationReady();
    b.world->rotationReady();
    net.until([&] { return phase(a) == RotationPhase::Committed && phase(b) == RotationPhase::Committed; }, 120);
    // Arriving after the commit, before the next map: refused as well.
    Node& committed = net.add(NetworkRole::Client, kHost + 12);
    net.until([&] { return committed.world->state() == NetworkState::Failed; }, 120);
    check(committed.world->failure() == ReasonCode::MapRotation, "a join after the commit is refused");
    // The next map: a newcomer gets in before the old players, and doesn't take their slots.
    net.restart(host);
    // (It found the lobby on the next map: sw_scene says so, and that's what it plays.)
    Node& newcomer = net.nodes.emplace_back();
    newcomer.services = std::make_unique<LoopbackOnlineServices>(*net.hub, PeerId{kHost + 13});
    NetworkStart fresh = net.start(NetworkRole::Client);
    fresh.compatibility.sceneId = 2;
    net.begin(newcomer, fresh);
    net.until([&] { return active(newcomer); }, 600);
    check(active(newcomer), "a newcomer joins the next map");
    check(
        newcomer.world->localSlot() != slotA && newcomer.world->localSlot() != slotB,
        "without taking a slot held for the last map's players");
    net.restart(a);
    net.restart(b);
    net.until([&] { return active(a) && active(b); }, 600);
    check(a.world->localSlot() == slotA && b.world->localSlot() == slotB, "the old players get their slots back");
}

// ---- Host loss ---------------------------------------------------------------------------------

void hostLoss() {
    {
        Net net;
        join(net, 1);
        Node &host = net.nodes[0], &a = net.nodes[1];
        check(bool(host.world->prepareRotation(nextMap(5))), "the host announces the next map");
        net.until([&] { return phase(a) == RotationPhase::Preparing; }, 60);
        host.world->close();
        host.world.reset();
        net.until([&] { return a.world->state() == NetworkState::Failed; }, 60 * 15);
        check(a.world->failure() == ReasonCode::HostLeft, "the host leaving during preparation ends the match");
    }
    {
        Net net;
        join(net, 1);
        Node &host = net.nodes[0], &a = net.nodes[1];
        check(bool(host.world->prepareRotation(nextMap(2))), "the host announces the next map");
        host.world->rotationReady();
        net.until([&] { return phase(a) == RotationPhase::Preparing; }, 60);
        a.world->rotationReady();
        net.until([&] { return phase(a) == RotationPhase::Committed; }, 120);
        host.world->close();   // and never starts the next map
        host.world.reset();
        net.run(10);
        check(a.world->state() == NetworkState::Active, "the session ending for the next map isn't a failure");
        net.restart(a);
        net.until([&] { return a.world->state() == NetworkState::Failed; }, 60 * 40);
        check(a.world->failure() == ReasonCode::Timeout, "a host that never starts the next map times out");
        check(a.world->stats().rejoinAttempts > 1, "after trying again for a while");
    }
    {
        // The host's next session isn't the one it announced.
        Net net;
        join(net, 1);
        Node &host = net.nodes[0], &a = net.nodes[1];
        check(bool(host.world->prepareRotation(nextMap(2))), "the host announces the next map");
        host.world->rotationReady();
        net.until([&] { return phase(a) == RotationPhase::Preparing; }, 60);
        a.world->rotationReady();
        net.until([&] { return phase(a) == RotationPhase::Committed; }, 120);
        NetworkStart other = host.world->nextStart();
        other.presetSession = false;   // a fresh, random nonce
        host.world->close();
        net.begin(host, other);
        net.restart(a);
        net.until([&] { return a.world->state() == NetworkState::Failed; }, 600);
        check(a.world->failure() == ReasonCode::MapNotLoaded, "a client won't join a session it wasn't told of");
    }
}

// ---- A raw peer: duplicate and stale answers ------------------------------------------------

struct RawClient final : TransportSink {
    std::unique_ptr<ITransport> transport;
    ConnectionId connection;
    Nonce session{};
    NetworkCompatibility compatibility;
    bool connected = false, closed = false, active = false;
    ReasonCode closedReason = ReasonCode::Normal;
    uint32_t prepares = 0, commits = 0, transition = 0, nextEpoch = 0;

    void send(const MessageBody& body) {
        std::array<uint8_t, kMaxReliableMessageBytes> out{};
        uint32_t written = 0;
        if (!encodeMessage(body, session, 0, out, written)) return;
        transport->send(connection, std::span(out.data(), written), findMessage(uint8_t(kindOf(body)))->delivery);
    }
    void onEvent(const TransportEvent& event) override {
        if (event.kind == TransportEventKind::Closed) {
            closed = true;
            closedReason = event.reason;
        }
        if (event.kind != TransportEventKind::Connected || connected) return;
        connected = true;
        Hello hello;
        hello.gameProtocol = compatibility.gameProtocol;
        hello.schemaDigest = compatibility.schemaDigest;
        hello.contentDigest = compatibility.contentDigest;
        hello.sceneId = compatibility.sceneId;
        send(hello);
    }
    void onMessage(const ReceivedMessage& message) override {
        DecodeOptions options;
        options.delivery = message.delivery;
        options.receiver = PeerRole::Client;
        DecodedMessage m;
        if (!decodeMessage(message.bytes, options, m)) return;
        switch (kindOf(m.body)) {
        case MessageKind::Challenge: session = m.header.session; break;
        case MessageKind::BaselineEnd: {
            const auto& end = std::get<BaselineEnd>(m.body);
            send(Ready{end.baselineId, end.baselineDigest});
            break;
        }
        case MessageKind::Active: active = true; break;
        case MessageKind::MapPrepare:
            ++prepares;
            transition = std::get<MapPrepare>(m.body).transitionId;
            nextEpoch = std::get<MapPrepare>(m.body).nextEpoch;
            break;
        case MessageKind::MapCommit: ++commits; break;
        default: break;
        }
    }
};

void duplicates() {
    Net net(false);   // the raw peer can't authenticate (loopback only)
    net.add(NetworkRole::Authority, kHost);
    Node& host = net.nodes[0];
    auto services = std::make_unique<LoopbackOnlineServices>(*net.hub, PeerId{kHost + 1});
    RawClient raw;
    raw.compatibility = net.start(NetworkRole::Client).compatibility;
    raw.transport = std::move(services->createTransport().value());
    raw.connection = raw.transport->connect(net.start(NetworkRole::Client).address).value();
    net.extra.push_back([&] { raw.transport->poll(raw, {}); });
    net.until([&] { return raw.active; }, 300);
    check(raw.active, "a raw peer joins by hand");
    check(bool(host.world->prepareRotation(nextMap(5))), "the host announces the next map");
    host.world->rotationReady();
    net.until([&] { return raw.prepares == 1; }, 60);
    // Ready, then the same again, then a reject for the same map after the commit.
    raw.send(MapReady{raw.transition, raw.nextEpoch, raw.compatibility.contentDigest});
    raw.send(MapReady{raw.transition, raw.nextEpoch, raw.compatibility.contentDigest});
    raw.send(MapReject{raw.transition, ReasonCode::MapNotLoaded});
    net.until([&] { return raw.commits >= 1; }, 60);
    net.run(5);
    check(phase(host) == RotationPhase::Committed, "the host commits on the first ready");
    check(raw.commits == 1, "the commit is sent once");
    check(host.world->rotation().duplicates == 2, "the repeated ready and the stale reject are ignored");
    check(!raw.closed, "and the peer isn't dropped for them");
    raw.send(MapReady{raw.transition + 5, raw.nextEpoch, raw.compatibility.contentDigest});
    net.until([&] { return raw.closed; }, 60);
    check(raw.closed && raw.closedReason == ReasonCode::MalformedPacket, "an answer to an unannounced map is refused");
    raw.transport.reset();
}

// ---- Through LevelHost: real Play sessions change maps -------------------------------------

struct Quiet final : GameInstance {
    std::unique_ptr<GameMode> createGameMode(const World&) override { return std::make_unique<GameMode>(); }
};

void mark(World& world, const std::string& level) {
    const Entity marker = world.create();
    world.add<Name>(marker, {"level " + level});
}

TravelRequest to(const char* level) {
    TravelRequest r;
    r.level = level;
    return r;
}

// One machine: a LevelHost whose levels "arena" (scene 1) and "factory" (scene 2) are
// empty scenes with a named marker, and its services.
struct Player {
    World world;
    Quiet instance;
    std::unique_ptr<LoopbackOnlineServices> services;
    std::unique_ptr<LevelHost> levels;
    std::optional<NetworkStart> first;

    Player(Net& net, JobSystem& jobs, uint64_t identity, NetworkRole role) {
        services = std::make_unique<LoopbackOnlineServices>(*net.hub, PeerId{identity});
        first = net.start(role);
        LevelHost::Hooks hooks;
        hooks.load = [](const LevelRequest& request, World& into, std::string&) {
            mark(into, request.level);
            return true;
        };
        hooks.networkScene = [](const std::string& level, std::string& error) -> uint16_t {
            if (level == "arena") return 1;
            if (level == "factory") return 2;
            error = "no such level";
            return 0;
        };
        hooks.configure = [this](PlaySession& session, const LevelRequest&) {
            session.setAudioDevice(false);
            if (first) session.setNetworkStart(*first, services.get());
            first.reset();   // later maps come from the rotation
        };
        levels = std::make_unique<LevelHost>(world, jobs, instance, nullptr, std::move(hooks));
        levels->setStaged(true);
        mark(world, "arena");
        levels->start({"arena"}, role == NetworkRole::Client ? StartReason::OnlineBaseline : StartReason::Fresh);
    }
    PlaySession* session() const { return levels->session(); }
    NetworkWorld* network() const { return session() ? session()->network() : nullptr; }
    bool activeOn(const std::string& level, uint32_t epoch) const {
        return network() && network()->state() == NetworkState::Active && network()->mapEpoch() == epoch &&
               levels->current().level == level;
    }
};

void throughLevelHost() {
    Net net;
    JobSystem jobs(2);
    Player host(net, jobs, kHost, NetworkRole::Authority);
    Player a(net, jobs, kHost + 1, NetworkRole::Client);
    Player b(net, jobs, kHost + 2, NetworkRole::Client);
    std::vector<Player*> all = {&host, &a, &b};
    auto frame = [&] {
        const double now = net.now();
        for (Player* p : all) {
            pumpNetworkFrame(p->services.get(), p->session(), {now, 1.0 / 60, false});
            if (PlaySession* s = p->session()) {
                s->update(1.0 / 60);
                s->frame(1.0 / 60);
            }
            p->levels->process();
        }
        net.hub->advance(16667);
    };
    auto until = [&](const std::function<bool()>& done, int frames) {
        for (int i = 0; i < frames && !done(); ++i) frame();
        return done();
    };
    check(until([&] { return a.activeOn("arena", 1) && b.activeOn("arena", 1); }, 600), "two clients join the host");
    const uint16_t slotA = a.network()->localSlot(), slotB = b.network()->localSlot();
    // A client can't change maps; the host can, for everyone.
    check(a.levels->request(to("factory")).error == TravelError::AuthorityRequired, "a client can't change maps");
    const TravelTicket ticket = host.levels->request(to("factory"));
    check(bool(ticket), "the host asks for the next map");
    check(
        until(
            [&] { return host.activeOn("factory", 2) && a.activeOn("factory", 2) && b.activeOn("factory", 2); }, 1200),
        "everyone plays the next map, as the next session");
    check(a.network()->localSlot() == slotA && b.network()->localSlot() == slotB, "in the same slots");
    check(findEntity(a.world, "level factory").entity() != kNullEntity, "a client's world is the next level");
    const auto status = host.levels->status(ticket.id);
    check(status && status->phase == TravelPhase::Succeeded, "the host's ticket succeeded");
    check(!host.levels->request(to("nowhere")), "a level that isn't a network scene isn't announced");
}

// ---- Many rotations, every fault preset ------------------------------------------------------

void scenarios() {
    for (const char* preset : {"clean", "cafe", "poor", "hostile"}) {
        NetTestOptions options;
        options.scenario = "map_rotation";
        options.preset = preset;
        options.clients = 5;
        options.rotations = 4;
        options.seed = 11;
        NetTestReport report;
        check(runNetTest(options, report), std::string("map_rotation runs on ") + preset);
        check(report.ok, std::string("map_rotation passes on ") + preset);
        for (const std::string& e : report.errors) std::fprintf(stderr, "  %s: %s\n", preset, e.c_str());
        check(report.rotation.rotations == 4, std::string("four rotations on ") + preset);
    }
}

}   // namespace

int main() {
    neverReady();
    abortThenRetry();
    clientRejects();
    lateJoins();
    hostLoss();
    duplicates();
    throughLevelHost();
    scenarios();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
