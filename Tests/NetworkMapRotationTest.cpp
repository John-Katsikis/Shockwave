// Map rotation (shipping plan S11, docs/Networking.md "Map rotation"): NetworkWorlds over an
// in-process LoopbackHub change maps by controlled session restart. Covered: a player who
// never gets ready (dropped, or the rotation called off), a player who can't load the map,
// joins in every phase, reserved slots, host loss before and after the commit, a next
// session that isn't the one announced, duplicate and stale answers from a raw peer, and
// many rotations in a row (the --net-test map_rotation scenario, on every fault preset).
// S11 part 2: repeated announcements and commits, other content and an old map's message
// reaching a client (through a proxy), 4 s and 12 s outages (also across a map change), a
// ticket refused on the next map, kicking, 64 players' caps, and the measured match at 2
// to 64 players on several fault presets, with update degradation on and off.
#include <Core/Jobs.h>
#include <Engine/Components.h>
#include <GameFramework/LevelHost.h>
#include <GameFramework/NetworkPump.h>
#include <Multiplayer/NetTest.h>
#include <Multiplayer/NetworkWorld.h>
#include <Network/LoopbackServices.h>
#include <Network/Messages.h>

#include <algorithm>
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

// ---- A proxy between a client and the host: what reaches the client --------------------------

// Forwards every message both ways. The client sees the proxy as the host, so the Challenge
// is encoded again naming it; everything else passes as it came, unless `onHost` (seeing
// each host message, decoded) says to drop it. inject() sends a message to the client.
struct Proxy {
    static constexpr uint64_t kIdentity = kHost + 500;
    struct Link {
        ConnectionId front, back;
        bool backUp = false;
        std::vector<std::pair<std::vector<uint8_t>, Delivery>> waiting;   // for the host
    };
    struct Side final : TransportSink {
        Proxy* proxy = nullptr;
        bool front = false;
        void onEvent(const TransportEvent& e) override { proxy->event(front, e); }
        void onMessage(const ReceivedMessage& m) override { proxy->message(front, m); }
    };
    std::unique_ptr<LoopbackOnlineServices> services;
    std::unique_ptr<ITransport> front, back;
    TransportAddress hostAt;
    std::vector<Link> links;
    Side frontSide, backSide;
    std::function<bool(const DecodedMessage&, std::span<const uint8_t>)> onHost;   // false: dropped

    Proxy(LoopbackHub& hub, TransportAddress listenAt, TransportAddress host) : hostAt(host) {
        services = std::make_unique<LoopbackOnlineServices>(hub, PeerId{kIdentity});
        front = std::move(services->createTransport().value());
        back = std::move(services->createTransport().value());
        check(bool(front->listen(listenAt, {})), "the proxy listens");
        frontSide.proxy = backSide.proxy = this;
        frontSide.front = true;
    }
    void poll() {
        front->poll(frontSide, {});
        back->poll(backSide, {});
    }
    Link* byFront(ConnectionId c) {
        for (Link& l : links)
            if (l.front == c) return &l;
        return nullptr;
    }
    Link* byBack(ConnectionId c) {
        for (Link& l : links)
            if (l.back == c) return &l;
        return nullptr;
    }
    void event(bool fromFront, const TransportEvent& e) {
        if (fromFront && e.kind == TransportEventKind::Incoming) {
            (void)front->accept(e.connection);
            if (auto c = back->connect(hostAt)) links.push_back({e.connection, c.value()});
            return;
        }
        if (!fromFront && e.kind == TransportEventKind::Connected) {
            if (Link* l = byBack(e.connection)) {
                l->backUp = true;
                for (auto& [bytes, delivery] : l->waiting) back->send(l->back, bytes, delivery);
                l->waiting.clear();
            }
            return;
        }
        if (e.kind != TransportEventKind::Closed) return;
        Link* l = fromFront ? byFront(e.connection) : byBack(e.connection);
        if (!l) return;
        if (fromFront)
            back->close(l->back, e.reason);
        else
            front->close(l->front, e.reason);
        std::erase_if(links, [&](const Link& x) { return &x == l; });
    }
    void message(bool fromFront, const ReceivedMessage& m) {
        if (fromFront) {
            if (Link* l = byFront(m.connection)) {
                if (l->backUp)
                    back->send(l->back, m.bytes, m.delivery);
                else
                    l->waiting.emplace_back(std::vector<uint8_t>(m.bytes.begin(), m.bytes.end()), m.delivery);
            }
            return;
        }
        Link* l = byBack(m.connection);
        if (!l) return;
        DecodeOptions options;
        options.delivery = m.delivery;
        options.receiver = PeerRole::Client;
        DecodedMessage d;
        if (!decodeMessage(m.bytes, options, d)) return;
        if (onHost && !onHost(d, m.bytes)) return;
        if (kindOf(d.body) == MessageKind::Challenge) {
            Challenge c = std::get<Challenge>(d.body);
            c.hostPeer = kIdentity;
            inject(c, d.header.session, d.header.senderTick, l);
            return;
        }
        front->send(l->front, m.bytes, m.delivery);
    }
    // To the client of the newest link (or `l`), encoded with this session.
    void inject(const MessageBody& body, const Nonce& session, uint32_t tick, Link* l = nullptr) {
        if (!l && !links.empty()) l = &links.back();
        if (!l) return;
        std::array<uint8_t, kMaxReliableMessageBytes> out{};
        uint32_t written = 0;
        if (!encodeMessage(body, session, tick, out, written)) return;
        front->send(l->front, std::span(out.data(), written), findMessage(uint8_t(kindOf(body)))->delivery);
    }
    void sendRaw(std::span<const uint8_t> bytes, Delivery delivery) {
        if (!links.empty()) front->send(links.back().front, bytes, delivery);
    }
};

// A host, one client through a proxy (unauthenticated: the proxy can't sign tickets), all
// Active.
struct Proxied {
    Net net{false};
    std::optional<Proxy> proxy;
    Node* host = nullptr;
    Node* client = nullptr;
    Proxied() {
        host = &net.add(NetworkRole::Authority, kHost);
        NetworkStart s = net.start(NetworkRole::Client);
        proxy.emplace(*net.hub, TransportAddress{TransportAddress::Loopback, 10, 0}, s.address);
        net.extra.push_back([this] { proxy->poll(); });
        s.address = {TransportAddress::Loopback, 10, 0};
        Node& n = net.nodes.emplace_back();
        n.services = std::make_unique<LoopbackOnlineServices>(*net.hub, PeerId{kHost + 1});
        net.begin(n, s);
        client = &n;
        net.until([&] { return active(*client) && host->world->stats().activePeers == 1; }, 300);
        check(active(*client), "a client joins through the proxy");
    }
};

void throughProxy() {
    {
        // The host's announcement and commit arrive twice: each is handled once.
        Proxied t;
        std::vector<std::pair<std::vector<uint8_t>, Delivery>> repeat;
        t.proxy->onHost = [&](const DecodedMessage& d, std::span<const uint8_t> bytes) {
            const MessageKind k = kindOf(d.body);
            if (k == MessageKind::MapPrepare || k == MessageKind::MapCommit)
                repeat.emplace_back(std::vector<uint8_t>(bytes.begin(), bytes.end()), Delivery::ReliableOrdered);
            return true;
        };
        t.net.extra.push_back([&] {
            for (auto& [bytes, delivery] : repeat) t.proxy->sendRaw(bytes, delivery);
            repeat.clear();
        });
        check(bool(t.host->world->prepareRotation(nextMap(5))), "the host announces the next map");
        t.host->world->rotationReady();
        t.net.until([&] { return phase(*t.client) == RotationPhase::Preparing; }, 120);
        t.net.run(10);
        check(t.client->world->rotation().duplicates == 1, "a repeated MapPrepare is ignored");
        t.client->world->rotationReady();
        t.net.until([&] { return phase(*t.client) == RotationPhase::Committed; }, 120);
        t.net.run(10);
        check(t.client->world->rotation().duplicates == 2, "so is a repeated MapCommit");
        check(active(*t.client), "and neither ends the session");
    }
    {
        // An announcement naming other content: the client refuses that map, the host
        // goes on without it.
        Proxied t;
        t.proxy->onHost = [&](const DecodedMessage& d, std::span<const uint8_t>) {
            if (kindOf(d.body) != MessageKind::MapPrepare) return true;
            MapPrepare p = std::get<MapPrepare>(d.body);
            p.contentDigest[5] ^= 0xff;
            t.proxy->inject(p, d.header.session, d.header.senderTick);
            return false;
        };
        check(bool(t.host->world->prepareRotation(nextMap(5))), "the host announces the next map");
        t.host->world->rotationReady();
        t.net.until([&] { return t.client->world->state() == NetworkState::Failed; }, 300);
        check(
            t.client->world->state() == NetworkState::Failed &&
                t.client->world->failure() == ReasonCode::ContentMismatch,
            "a map announced with other content is refused with ContentMismatch");
        t.net.until([&] { return phase(*t.host) == RotationPhase::Committed; }, 60);
        check(phase(*t.host) == RotationPhase::Committed, "the host goes on without that player");
    }
    {
        // A message from the old map's session (a snapshot or ping: these sessions have no
        // world) arriving on the next map's connection.
        Proxied t;
        std::vector<uint8_t> oldMessage;
        t.proxy->onHost = [&](const DecodedMessage& d, std::span<const uint8_t> bytes) {
            if (kindOf(d.body) == MessageKind::Snapshot || kindOf(d.body) == MessageKind::Ping)
                oldMessage.assign(bytes.begin(), bytes.end());
            return true;
        };
        t.net.run(130);   // the host pings each client once a second
        check(!oldMessage.empty(), "the proxy kept a message from the old map");
        check(bool(t.host->world->prepareRotation(nextMap(5))), "the host announces the next map");
        t.host->world->rotationReady();
        t.net.until([&] { return phase(*t.client) == RotationPhase::Preparing; }, 120);
        t.proxy->onHost = nullptr;
        t.client->world->rotationReady();
        t.net.until([&] { return phase(*t.client) == RotationPhase::Committed; }, 120);
        t.net.until([&] { return t.host->world->rotationHandedOver(); }, 300);
        t.net.restart(*t.host);
        t.net.restart(*t.client);
        t.net.until([&] { return active(*t.client) && t.client->world->mapEpoch() == 2; }, 600);
        check(active(*t.client) && t.client->world->mapEpoch() == 2, "the client is on the next map");
        const uint64_t applied = t.client->world->stats().recordsApplied;
        t.proxy->sendRaw(oldMessage, Delivery::Unreliable);
        t.net.run(10);
        check(
            t.client->world->state() == NetworkState::Failed &&
                t.client->world->failure() == ReasonCode::MalformedPacket,
            "a message from the old map's session on the new connection is refused, never applied");
        check(t.client->world->stats().recordsApplied == applied, "nothing in it was applied");
    }
}

// ---- Outages and authentication ----------------------------------------------------------------

void outages() {
    {
        // 4 s without any traffic: shorter than idle_timeout, so everyone stays.
        Net net;
        join(net, 3);
        net.hub->addOutage(net.hub->nowUs(), 4'000'000);
        net.run(60 * 6);
        bool all = true;
        for (const Node& n : net.nodes) all = all && active(n);
        check(all, "a 4 s outage: everyone is still in the match");
        check(net.nodes[0].world->stats().activePeers == 3, "the host still has every player");
    }
    {
        // 12 s: longer than idle_timeout (10 s). Every client times out; the host keeps
        // hosting, gives the slots back and lets players in again.
        Net net;
        join(net, 3);
        net.hub->addOutage(net.hub->nowUs(), 12'000'000);
        net.run(60 * 17);
        bool timedOut = true;
        for (size_t i = 1; i < net.nodes.size(); ++i)
            timedOut = timedOut && net.nodes[i].world->state() == NetworkState::Failed &&
                       net.nodes[i].world->failure() == ReasonCode::Timeout;
        check(timedOut, "a 12 s outage: every client fails with Timeout");
        Node& host = net.nodes[0];
        check(active(host) && host.world->stats().admitted == 0, "the host keeps hosting with every slot free");
        check(host.services->openAuthSessions() == 0, "and no auth session left open");
        net.nodes[1].world->close();
        net.begin(net.nodes[1], net.start(NetworkRole::Client));
        net.until([&] { return active(net.nodes[1]); }, 600);
        check(active(net.nodes[1]), "a player joins again after the outage");
    }
    {
        // 12 s across a map change: the host can't hear MapReady, drops the players at the
        // deadline and starts the next map alone; the clients time out cleanly.
        Net net;
        join(net, 2);
        Node& host = net.nodes[0];
        check(bool(host.world->prepareRotation(nextMap(3))), "the host announces the next map");
        host.world->rotationReady();
        net.until([&] { return phase(net.nodes[1]) == RotationPhase::Preparing; }, 120);
        net.hub->addOutage(net.hub->nowUs(), 12'000'000);
        for (size_t i = 1; i < net.nodes.size(); ++i) net.nodes[i].world->rotationReady();
        net.until([&] { return phase(host) != RotationPhase::Preparing; }, 60 * 6);
        check(phase(host) == RotationPhase::Committed, "the host goes on at the deadline");
        check(host.world->stats().rotationDrops == 2, "without the players it couldn't hear");
        net.run(60 * 17);
        for (size_t i = 1; i < net.nodes.size(); ++i)
            check(
                net.nodes[i].world->state() == NetworkState::Failed,
                "a client cut off during the map change leaves the match");
        net.nodes[0].world->close();
        for (size_t i = 1; i < net.nodes.size(); ++i) net.nodes[i].world->close();
        net.run(30);
        for (const Node& n : net.nodes)
            check(n.services->openAuthSessions() == 0 && n.services->liveTickets() == 0, "nothing left open after");
    }
}

void reauthentication() {
    // The next map authenticates everyone again. A player whose ticket is refused then
    // leaves with AuthenticationFailed; the others play on in their slots.
    Net net;
    join(net, 2);
    Node &host = net.nodes[0], &a = net.nodes[1], &b = net.nodes[2];
    const uint16_t slotA = a.world->localSlot();
    check(bool(host.world->prepareRotation(nextMap(5))), "the host announces the next map");
    host.world->rotationReady();
    net.until([&] { return phase(a) == RotationPhase::Preparing && phase(b) == RotationPhase::Preparing; }, 120);
    a.world->rotationReady();
    b.world->rotationReady();
    net.until([&] { return phase(a) == RotationPhase::Committed && phase(b) == RotationPhase::Committed; }, 120);
    net.until([&] { return host.world->rotationHandedOver(); }, 300);
    net.restart(host);
    net.restart(a);
    net.until([&] { return active(a) && a.world->mapEpoch() == 2; }, 600);
    check(active(a) && a.world->localSlot() == slotA, "the first player is back in its slot");
    host.services->setValidation(LoopbackOnlineServices::Validation::Refuse);
    net.restart(b);
    net.until([&] { return b.world->state() == NetworkState::Failed; }, 600);
    check(
        b.world->state() == NetworkState::Failed && b.world->failure() == ReasonCode::AuthenticationFailed,
        "a player whose ticket is refused on the next map leaves with AuthenticationFailed");
    check(active(a) && host.world->stats().admitted == 1, "the others play on");
    host.world->close();
    a.world->close();
    b.world->close();
    net.run(30);
    for (const Node& n : net.nodes)
        check(n.services->openAuthSessions() == 0 && n.services->liveTickets() == 0, "no auth session left open");
}

// ---- Kicking --------------------------------------------------------------------------------

void kicking() {
    Net net;
    join(net, 2);
    Node &host = net.nodes[0], &a = net.nodes[1];
    const uint16_t slot = a.world->localSlot();
    check(!a.world->kick(1), "a client can't kick anyone");
    check(host.world->kick(slot), "the host kicks a player");
    net.until([&] { return a.world->state() == NetworkState::Failed; }, 120);
    check(a.world->failure() == ReasonCode::Kicked, "who leaves with Kicked");
    check(host.world->stats().admitted == 1, "and frees the slot");
    net.begin(a, net.start(NetworkRole::Client));
    net.until([&] { return a.world->state() == NetworkState::Failed; }, 300);
    check(a.world->failure() == ReasonCode::Kicked, "a kicked player can't come back to the match");
    // Not on the next map either.
    Node& b = net.nodes[2];
    check(bool(host.world->prepareRotation(nextMap(5))), "the host announces the next map");
    host.world->rotationReady();
    net.until([&] { return phase(b) == RotationPhase::Preparing; }, 120);
    b.world->rotationReady();
    net.until([&] { return phase(b) == RotationPhase::Committed; }, 120);
    net.until([&] { return host.world->rotationHandedOver(); }, 300);
    check(host.world->nextStart().kicked.size() == 1, "the kick carries to the next map");
    net.restart(host);
    net.restart(b);
    net.until([&] { return active(b); }, 600);
    net.begin(a, net.start(NetworkRole::Client));
    net.until([&] { return a.world->state() == NetworkState::Failed; }, 300);
    check(a.world->failure() == ReasonCode::Kicked, "and is refused there too");
}

// ---- 64 players: resource caps -----------------------------------------------------------------

void sixtyFourPlayers() {
    Net net;
    // join() for 63 clients at max_players 64.
    auto start = [&](NetworkRole role) {
        NetworkStart s = net.start(role);
        s.settings.maxPlayers = 64;
        return s;
    };
    auto add = [&](NetworkRole role, uint64_t id) -> Node& {
        Node& n = net.nodes.emplace_back();
        n.services = std::make_unique<LoopbackOnlineServices>(*net.hub, PeerId{id});
        net.begin(n, start(role));
        return n;
    };
    Node& host = add(NetworkRole::Authority, kHost);
    for (uint64_t i = 1; i <= 63; ++i) add(NetworkRole::Client, kHost + i);
    net.until([&] { return host.world->stats().activePeers == 63; }, 60 * 20);
    check(host.world->stats().activePeers == 63, "63 clients join a 64-player host");
    std::vector<uint16_t> slots;
    for (size_t i = 1; i < net.nodes.size(); ++i) slots.push_back(net.nodes[i].world->localSlot());
    std::sort(slots.begin(), slots.end());
    check(
        slots.front() == 2 && slots.back() == 64 && std::adjacent_find(slots.begin(), slots.end()) == slots.end(),
        "each in its own slot, 2 to 64");
    Node& extra = add(NetworkRole::Client, kHost + 64);
    net.until([&] { return extra.world->state() == NetworkState::Failed; }, 600);
    check(extra.world->failure() == ReasonCode::ServerFull, "the 65th player is refused with ServerFull");
    check(host.world->memoryBytes() < 8u * 1024 * 1024, "the host's session buffers stay under 8 MiB at 64 players");
    std::vector<NetworkPeerInfo> info;
    host.world->peerInfo(info);
    check(info.size() == 63, "the host lists 63 players");
    // A map change with everyone.
    check(bool(host.world->prepareRotation(nextMap(10))), "the host announces the next map to 63 players");
    host.world->rotationReady();
    net.until(
        [&] {
            bool all = true;
            for (size_t i = 1; i < net.nodes.size(); ++i) {
                if (phase(net.nodes[i]) == RotationPhase::Preparing) net.nodes[i].world->rotationReady();
                all = all && (i == net.nodes.size() - 1 || phase(net.nodes[i]) == RotationPhase::Committed);
            }
            return all;
        },
        60 * 5);
    check(phase(host) == RotationPhase::Committed, "and commits once all 63 are ready");
    check(host.world->stats().rotationDrops == 0, "without dropping anyone");
}

// ---- The measured match scenario ----------------------------------------------------------------

void matches() {
    struct Case {
        const char* preset;
        uint32_t clients;
    };
    for (const Case& c : {Case{"cafe", 1}, Case{"hostile", 11}, Case{"poor", 31}, Case{"cafe", 63}}) {
        NetTestOptions options;
        options.scenario = "match";
        options.preset = c.preset;
        options.clients = c.clients;
        options.settings.maxPlayers = 64;
        options.seed = 5;
        NetTestReport report;
        const std::string tag = std::string(" (") + std::to_string(c.clients + 1) + " players, " + c.preset + ")";
        check(runNetTest(options, report), "the match scenario runs" + tag);
        check(report.ok, "and passes" + tag);
        for (const std::string& e : report.errors) std::fprintf(stderr, "  %s: %s\n", tag.c_str(), e.c_str());
        const NetTestMatch& m = report.match;
        check(m.players == c.clients + 1 && m.shots > 0 && m.scoreEvents > 0, "players move, shoot and score" + tag);
        check(m.hostUpPerClient <= 1.1 * options.settings.outboundBytesPerSecond, "within the bandwidth budget" + tag);
        if (c.clients == 63) {
            check(m.degradedPeersHigh > 0, "64 players need more than the budget: updates degrade" + tag);
            check(m.updateLevelDrops == 0, "and settle instead of going up and down" + tag);
        }
    }
    // Off, nothing degrades: what's over budget is held back round-robin, and the match
    // still ends with every character shown where it is.
    NetTestOptions options;
    options.scenario = "match";
    options.preset = "cafe";
    options.clients = 63;
    options.settings.maxPlayers = 64;
    options.settings.degradeUpdates = false;
    NetTestReport report;
    check(runNetTest(options, report) && report.ok, "degrade_updates 0: the 64-player match still passes");
    for (const std::string& e : report.errors) std::fprintf(stderr, "  degrade off: %s\n", e.c_str());
    check(
        report.match.degradedPeersHigh == 0 && report.match.recordsLimited > 0,
        "nothing degrades, records wait instead");
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
    throughProxy();
    outages();
    reauthentication();
    kicking();
    sixtyFourPlayers();
    matches();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
