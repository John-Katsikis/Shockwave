// OnlineMatch (Multiplayer/OnlineMatch.h) over SteamServices and MockSteamApi, with real
// NetworkWorlds: host -> lobby metadata -> Active -> joinable; find -> compatible listing;
// join -> metadata checks -> the client plays the owner's match; invite; leave; a
// mismatched build refused before connecting; an invite accepted while idle and while
// playing; developer direct joins; no services; a packet capture of the client's session
// (tickets redacted, peers opaque) and the Network window's peer table.
#include <Multiplayer/OnlineMatch.h>
#include <Multiplayer/Replication.h>
#include <Network/CaptureTransport.h>
#include <Network/Messages.h>
#include <Network/NetworkCapture.h>
#include <SteamServices/MockSteamApi.h>
#include <SteamServices/SteamServices.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace shockwave;
using namespace shockwave::net;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static constexpr uint64_t kHostId = 76561197960287930ull, kClientId = 76561197960287931ull,
                          kOtherId = 76561197960287932ull;

static const ReplicationRegistry& registry() {
    static ReplicationRegistry r = [] {
        ReplicationRegistry made;
        registerEngineReplication(made);
        std::vector<std::string> problems;
        made.freeze(problems);
        return made;
    }();
    return r;
}

static MatchSetup setup() {
    MatchSetup s;
    s.game = "arena";
    s.settings.enabled = true;
    s.settings.transport = NetTransportKind::Steam;
    s.settings.virtualPort = 7;
    s.settings.maxPlayers = 12;
    s.compatibility.gameProtocol = "arena-1";
    s.compatibility.schemaDigest = registry().schemaDigest();
    s.compatibility.contentDigest.fill(0x5A);
    s.compatibility.sceneId = 2;
    s.scenePath = "Scenes/arena-mp.swscene";
    s.registry = &registry();
    return s;
}

// One machine: Steam (mocked), the match, and the session the application would play.
struct Machine {
    MockSteamApi* mock = nullptr;
    std::unique_ptr<SteamServices> services;
    OnlineMatch match;
    World world;
    std::unique_ptr<NetworkWorld> session;
    CaptureWriter* capture = nullptr;
    bool signedIn = true;

    Machine(MockSteamNetwork& network, uint64_t steamId, MatchSetup s = setup()) {
        auto api = network.makeApi(steamId);
        mock = api.get();
        services = std::make_unique<SteamServices>(std::move(api));
        SteamServicesSettings steam;
        steam.appId = 480;
        steam.virtualPort = 7;
        CHECK(bool(services->initializeClient(steam)));
        match.setSetup(std::move(s));
        match.setServiceProvider([this](std::string& error) -> IOnlineServices* {
            if (!signedIn) {
                error = "Steam isn't running";
                return nullptr;
            }
            return services.get();
        });
    }
    ~Machine() { stopPlay(); }

    void stopPlay() {
        if (session) session->close();
        session.reset();
    }
    // What the application does each frame (GameFramework/NetworkPump.h).
    void frame(double now, uint32_t tick) {
        services->framePass();
        OnlineEvent event;
        while (services->pollEvent(event))
            if (!match.handleOnlineEvent(event) && session) session->handleOnlineEvent(event);
        if (session) {
            session->pump(now);
            session->drain(tick);
            session->endTick(tick);
        }
        match.update(session.get());
        if (match.takeEnded()) stopPlay();
        NetworkStart start;
        if (match.takeStart(start)) {
            stopPlay();
            start.capture = capture;
            session = std::make_unique<NetworkWorld>(start, services.get(), &world);
            CHECK(bool(session->begin()));
        }
    }
};

struct Sim {
    MockSteamNetwork network;
    std::vector<Machine*> machines;
    uint32_t tick = 0;
    void step(uint32_t ticks = 1) {
        for (uint32_t i = 0; i < ticks; ++i) {
            const double now = double(network.nowUs()) / 1e6;
            for (Machine* m : machines) m->frame(now, tick);
            network.advance(1000000 / 60);
            ++tick;
        }
    }
    template <class F>
    bool until(F done, uint32_t maxTicks = 600) {
        for (uint32_t i = 0; i < maxTicks; ++i) {
            if (done()) return true;
            step();
        }
        return done();
    }
};

static std::string lobbyValue(const LobbySummary& lobby, const char* key) {
    for (const auto& [k, v] : lobby.metadata)
        if (k == key) return v;
    return {};
}

static void testHostFindJoin() {
    Sim sim;
    Machine host(sim.network, kHostId), client(sim.network, kClientId);
    sim.machines = {&host, &client};

    CHECK(!client.match.invite());   // nothing to invite to
    CHECK(host.match.host());
    CHECK(host.match.state() == MatchState::CreatingLobby && host.match.busy());
    CHECK(!host.match.host());   // one match at a time
    CHECK(host.match.error().find("leave the current match") != std::string::npos);
    sim.step();
    CHECK(host.match.lobby().valid());
    // Not joinable until the session listens: a search finds nothing yet.
    CHECK(client.match.find());
    sim.step(2);
    CHECK(!client.match.searching());
    sim.until([&] { return host.match.state() == MatchState::Hosting; }, 10);
    CHECK(host.match.state() == MatchState::Hosting);
    CHECK(host.session && host.session->role() == NetworkRole::Authority);
    CHECK(host.session->start().lobby == host.match.lobby());

    CHECK(client.match.find());
    CHECK(client.match.find());   // a second request while one is in flight is the same search
    sim.until([&] { return !client.match.searching(); }, 10);
    CHECK(client.match.lobbies().size() == 1);
    if (client.match.lobbies().size() != 1) return;
    const LobbyListing& listing = client.match.lobbies().front();
    CHECK(listing.compatible && listing.problem.empty());
    CHECK(listing.host == PeerId{kHostId} && listing.id == host.match.lobby());
    CHECK(listing.hostName == "mock 930");
    CHECK(listing.players == 1 && listing.capacity == 12);
    CHECK(listing.scene == "2" && listing.state == "waiting");

    CHECK(client.match.join(listing.id));
    CHECK(client.match.state() == MatchState::JoiningLobby);
    sim.until([&] { return client.match.state() == MatchState::Joined; });
    CHECK(client.match.state() == MatchState::Joined);
    CHECK(client.session && client.session->state() == NetworkState::Active);
    CHECK(client.session->start().address.value == kHostId && client.session->start().lobby == listing.id);
    CHECK(client.session->localPlayer() == 2);
    CHECK(client.match.status().find("player 2") != std::string::npos);

    // The host advertises its player count; the client reads it in the lobby.
    sim.step(2);
    const NetResult<LobbySummary> info = client.services->lobbyInfo(listing.id);
    CHECK(info && lobbyValue(info.value(), "sw_players") == "2");
    CHECK(info && lobbyValue(info.value(), "sw_host") == std::to_string(kHostId));
    CHECK(info && lobbyValue(info.value(), "sw_session").size() == 32);
    CHECK(info && lobbyValue(info.value(), "sw_protocol") == "1");

    // The Network window's peer table, both ways.
    sim.step(130);   // the host pings each client once a second
    std::vector<NetworkPeerInfo> peers;
    host.session->peerInfo(peers);
    CHECK(peers.size() == 1);
    if (peers.size() == 1) {
        CHECK(peers[0].slot == 2 && std::strcmp(peers[0].status, "active") == 0);
        CHECK(peers[0].peer == PeerId{kClientId});
        CHECK(peers[0].bytesSent > 0 && peers[0].bytesReceived > 0);
        CHECK(peers[0].rttMs >= 0);
    }
    client.session->peerInfo(peers);
    CHECK(peers.size() == 1 && peers[0].peer == PeerId{kHostId});

    CHECK(host.match.invite());
    CHECK(host.mock->counts().invites == 1);
    CHECK(client.match.invite());

    // The client leaves: its lobby too, and the host lets its slot go.
    client.match.leave();
    sim.step();
    CHECK(!client.session && client.match.state() == MatchState::Idle);
    sim.until([&] { return host.session->stats().admitted == 0; }, 300);
    CHECK(host.session->stats().admitted == 0);
    sim.step(2);
    const NetResult<std::vector<PeerId>> members = host.services->lobbyMembers(host.match.lobby());
    CHECK(members && members->size() == 1);

    // The host leaves: Play stops, and the lobby is gone from searches.
    host.match.leave();
    sim.step(3);
    CHECK(!host.session && host.match.state() == MatchState::Idle && !host.match.lobby().valid());
    CHECK(client.match.find());
    sim.until([&] { return !client.match.searching(); }, 10);
    CHECK(client.match.lobbies().empty());
}

static void testMismatchRefused() {
    Sim sim;
    MatchSetup other = setup();
    other.compatibility.contentDigest.fill(0x11);
    Machine host(sim.network, kHostId), client(sim.network, kClientId, other);
    sim.machines = {&host, &client};
    CHECK(host.match.host());
    sim.until([&] { return host.match.state() == MatchState::Hosting; }, 20);
    CHECK(client.match.find());
    sim.until([&] { return !client.match.searching(); }, 10);
    CHECK(client.match.lobbies().size() == 1);
    if (client.match.lobbies().empty()) return;
    CHECK(!client.match.lobbies()[0].compatible);
    CHECK(client.match.lobbies()[0].problem.find("different content") != std::string::npos);
    // Joining anyway: refused from the lobby's details, before any connection.
    CHECK(client.match.join(client.match.lobbies()[0].id));
    sim.until([&] { return client.match.state() == MatchState::Failed; }, 20);
    CHECK(client.match.state() == MatchState::Failed);
    CHECK(client.match.error().find("can't join: it has different content") == 0);
    CHECK(!client.session);
    CHECK(client.mock->counts().openConnections == 0);
    sim.step(2);
    const NetResult<std::vector<PeerId>> members = host.services->lobbyMembers(host.match.lobby());
    CHECK(members && members->size() == 1);   // it left the lobby again
    CHECK(!client.match.takeEnded());   // Play never started
    // Failed until the next request, which starts over.
    CHECK(client.match.find());
}

static void testInviteAccepted() {
    Sim sim;
    Machine host(sim.network, kHostId), client(sim.network, kClientId), other(sim.network, kOtherId);
    sim.machines = {&host, &client, &other};
    CHECK(host.match.host());
    sim.until([&] { return host.match.state() == MatchState::Hosting; }, 20);
    // Idle: an accepted invite joins at once.
    client.mock->injectJoinRequest(host.match.lobby().value);
    sim.until([&] { return client.match.state() == MatchState::Joined; });
    CHECK(client.match.state() == MatchState::Joined);

    // Playing: a second host's invite waits for the player.
    CHECK(other.match.host());
    sim.until([&] { return other.match.state() == MatchState::Hosting; }, 20);
    client.mock->injectJoinRequest(other.match.lobby().value);
    sim.step(2);
    CHECK(client.match.pendingJoin() == other.match.lobby());
    CHECK(client.match.state() == MatchState::Joined);   // never replaced from a callback
    CHECK(client.session && client.session->start().lobby == host.match.lobby());
    client.match.declinePendingJoin();
    CHECK(!client.match.pendingJoin().valid());
    client.mock->injectJoinRequest(other.match.lobby().value);
    sim.step(2);
    CHECK(client.match.acceptPendingJoin());
    sim.until([&] { return client.match.state() == MatchState::Joined; });
    CHECK(client.session && client.session->start().lobby == other.match.lobby());
}

static void testDirectAndUnavailable() {
    Sim sim;
    Machine host(sim.network, kHostId), client(sim.network, kClientId);
    sim.machines = {&host, &client};
    // A lobby host admits only lobby members, so direct joins need a direct host.
    CHECK(host.match.hostDirect());
    sim.until([&] { return host.match.state() == MatchState::Hosting; }, 10);
    CHECK(host.match.directOnly() && !host.match.lobby().valid());
    CHECK(!host.match.invite());
    CHECK(client.match.joinDirect(PeerId{kHostId}));
    sim.until([&] { return client.match.state() == MatchState::Joined; });
    CHECK(client.match.state() == MatchState::Joined && !client.match.lobby().valid());
    CHECK(client.session && client.session->start().authenticate);   // still authenticated
    CHECK(!client.match.joinDirect(PeerId{0}));

    // The host goes: the client's match ends and Play stops.
    host.match.leave();
    sim.until([&] { return client.match.state() == MatchState::Failed; }, 600);
    CHECK(client.match.state() == MatchState::Failed);
    CHECK(client.match.error().rfind("disconnected: ", 0) == 0);
    CHECK(!client.session);

    // No services: visible failures, nothing in flight.
    Machine offline(sim.network, kOtherId);
    offline.signedIn = false;
    CHECK(!offline.match.host());
    CHECK(offline.match.error() == "can't host: Steam isn't running");
    CHECK(!offline.match.busy() && !offline.match.services());
    OnlineMatch unconfigured;
    CHECK(!unconfigured.host() && unconfigured.error().find("no network scene") != std::string::npos);
    CHECK(!unconfigured.join(LobbyId{0}));
}

static void testCaptureAndRedaction() {
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "online_match_test.swnetcap";
    Sim sim;
    CaptureWriter writer;
    CHECK(bool(writer.open(file)));
    Machine host(sim.network, kHostId), client(sim.network, kClientId);
    client.capture = &writer;
    sim.machines = {&host, &client};
    CHECK(host.match.host());
    sim.until([&] { return host.match.state() == MatchState::Hosting; }, 20);
    CHECK(client.match.find());
    sim.until([&] { return !client.match.searching(); }, 10);
    CHECK(!client.match.lobbies().empty());
    if (client.match.lobbies().empty()) return;
    CHECK(client.match.join(client.match.lobbies()[0].id));
    sim.until([&] { return client.match.state() == MatchState::Joined; });
    sim.step(30);
    const CaptureCounters* counters = client.session ? captureCounters(*client.session->transport()) : nullptr;
    CHECK(counters && counters->records > 10 && counters->redacted == 2 && counters->dropped == 0);
    client.match.leave();
    sim.step();
    writer.close();

    std::ifstream in(file, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    CaptureReader reader(bytes);
    CaptureRecord record;
    uint32_t records = 0, redacted = 0, sent = 0, received = 0;
    bool opaquePeers = true;
    while (reader.next(record)) {
        ++records;
        redacted += record.redacted();
        sent += record.direction == CaptureDirection::Sent;
        received += record.direction == CaptureDirection::Received;
        opaquePeers = opaquePeers && record.peer <= 1;   // one peer, the host: "1", never its Steam ID
    }
    CHECK(!reader.failed() && records > 10 && redacted == 2 && sent > 0 && received > 0 && opaquePeers);
    std::string json, error;
    CHECK(captureToJson(bytes, json, error));
    CHECK(json.find("Hello") != std::string::npos && json.find("\"redacted\": true") != std::string::npos);
    CHECK(json.find("\"peer\": \"1\"") != std::string::npos);
    CHECK(json.find("\"peer\": \"" + std::to_string(kHostId)) == std::string::npos);
    std::vector<uint8_t> again;
    CHECK(captureFromJson(json, again, error) && again == bytes);
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

int main() {
    testHostFindJoin();
    testMismatchRefused();
    testInviteAccepted();
    testDirectAndUnavailable();
    testCaptureAndRedaction();
    if (failures) {
        std::fprintf(stderr, "online_match: %d check(s) failed\n", failures);
        return 1;
    }
    std::puts("online_match: ok");
    return 0;
}
