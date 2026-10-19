// SteamServices (SteamServices/SteamServices.h) without Steam: the real factory in a build
// without the Steamworks SDK fails visibly and allocates nothing Steam, then everything
// else runs against MockSteamApi. Lifecycle: init failure, settings, double init, a
// dedicated server, shutdown exactly once, an idle framePass that allocates nothing.
// Lobbies: create, metadata, find, join, members, leave, late results after cancel.
// Authentication: tickets both ways, success, a ticket made for someone else, sizes,
// duplicates, revocation, cancellation, the 15-second fail-closed timeout, stale results.
#include <SteamServices/MockSteamApi.h>
#include <SteamServices/SteamServices.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

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

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static constexpr uint64_t kHostId = 76561197960287930ull, kClientId = 76561197960287931ull;
static constexpr uint32_t kAppId = 480;

static SteamServicesSettings settings() {
    SteamServicesSettings s;
    s.appId = kAppId;
    s.virtualPort = 7;
    return s;
}

// A mock-backed service; `mock` stays valid while `services` lives.
struct Node {
    MockSteamApi* mock = nullptr;
    std::unique_ptr<SteamServices> services;
    std::vector<OnlineEvent> events;

    Node(MockSteamNetwork& network, uint64_t steamId) {
        std::unique_ptr<MockSteamApi> api = network.makeApi(steamId);
        mock = api.get();
        services = std::make_unique<SteamServices>(std::move(api));
    }
    bool start() { return bool(services->initializeClient(settings())); }
    void pump() {
        services->framePass();
        OnlineEvent e;
        while (services->pollEvent(e)) events.push_back(std::move(e));
    }
    const OnlineEvent* find(OnlineEventKind kind) const {
        for (const OnlineEvent& e : events)
            if (e.kind == kind) return &e;
        return nullptr;
    }
    size_t count(OnlineEventKind kind) const {
        size_t n = 0;
        for (const OnlineEvent& e : events) n += e.kind == kind;
        return n;
    }
};

static void testNoSdkFactory() {
    const SteamBuild build = steamBuild();
#ifndef SHOCKWAVE_TEST_STEAM_SDK
    CHECK(!build.sdk);
#endif
    if (build.sdk) return;   // a Steam build: the manual gate covers the real SDK
    std::unique_ptr<SteamServices> services = SteamServices::create();
    CHECK(services->state() == SteamServiceState::Off);
    const NetResult<void> client = services->initializeClient(settings());
    CHECK(!client && client.error().code == NetErrorCode::Unavailable);
    CHECK(std::strstr(client.error().message, "SHOCKWAVE_STEAMWORKS") != nullptr);
    CHECK(!services->initializeDedicatedServer(settings(), {}));
    CHECK(!services->capabilities().steamworksBuilt && !services->available());
    CHECK(!services->createTransport());
    CHECK(!services->createLobby({}));
    CHECK(!services->requestAuthTicket(PeerId{kHostId}));
    services->framePass();
    services->shutdown();
    CHECK(services->state() == SteamServiceState::Off);
}

static void testLifecycle() {
    MockSteamNetwork network;
    {
        Node node(network, kHostId);
        node.mock->faults().failInit = true;
        const NetResult<void> failed = node.services->initializeClient(settings());
        CHECK(!failed && failed.error().code == NetErrorCode::Unavailable);
        CHECK(std::strstr(node.services->lastError(), "mock") != nullptr);
        CHECK(node.services->state() == SteamServiceState::Off);
        CHECK(node.mock->counts().inits == 1 && node.mock->counts().shutdowns == 0);
        node.services->shutdown();   // nothing to shut down
        CHECK(node.mock->counts().shutdowns == 0);

        node.mock->faults().failInit = false;
        SteamServicesSettings zero = settings();
        zero.appId = 0;
        CHECK(node.services->initializeClient(zero).error().code == NetErrorCode::InvalidArgument);
        SteamServicesSettings big = settings();
        big.maxUnreliableBytes = 1153;
        CHECK(node.services->initializeClient(big).error().code == NetErrorCode::InvalidArgument);
        CHECK(node.mock->counts().inits == 1);   // rejected before Steam was touched

        CHECK(node.start());
        const uint32_t generation = node.services->generation();
        CHECK(node.services->initializeClient(settings()).error().code == NetErrorCode::InvalidState);
        CHECK(node.services->state() == SteamServiceState::Client);
        CHECK(node.services->localPeer() == PeerId{kHostId});
        CHECK(!node.services->capabilities().steamworksBuilt);   // a mock isn't the SDK
        node.pump();
        CHECK(node.services->available());
        CHECK(node.services->relayStatus() == SteamRelayStatus::Current);

        // An idle frame allocates nothing.
        const uint64_t before = g_allocations.load();
        for (int i = 0; i < 1000; ++i) node.services->framePass();
        CHECK(g_allocations.load() == before);

        node.services->shutdown();
        node.services->shutdown();
        CHECK(node.mock->counts().shutdowns == 1);
        CHECK(node.services->localPeer() == PeerId{});
        CHECK(!node.services->available());

        // Started again: a new generation, and requests from the old one mean nothing.
        CHECK(node.start());
        CHECK(node.services->generation() == generation + 1);
    }
    // Not signed in; the destructor shuts it down.
    {
        Node node(network, kHostId);
        node.mock->faults().loggedOn = false;
        CHECK(node.start());
        node.pump();
        CHECK(!node.services->available());   // running, but not signed in
    }
}

static void testDedicatedServer() {
    MockSteamNetwork network;
    Node server(network, kHostId);
    SteamServerParams params;
    params.gamePort = 0;
    CHECK(server.services->initializeDedicatedServer(settings(), params).error().code == NetErrorCode::InvalidArgument);
    params.gamePort = 27015;
    CHECK(bool(server.services->initializeDedicatedServer(settings(), params)));
    CHECK(server.services->state() == SteamServiceState::DedicatedServer);
    const NetResult<RequestId> lobby = server.services->createLobby({});
    CHECK(!lobby && std::strstr(lobby.error().message, "dedicated server") != nullptr);
    CHECK(!server.services->findLobbies({}));
    CHECK(bool(server.services->createTransport()));   // it listens and authenticates
    CHECK(bool(server.services->requestAuthTicket(PeerId{kClientId})));
    server.services->shutdown();
    CHECK(server.mock->counts().shutdowns == 1);
}

static void testLobbies() {
    MockSteamNetwork network;
    Node host(network, kHostId), client(network, kClientId);
    CHECK(host.start() && client.start());
    CHECK(host.services->createLobby({LobbyVisibility::Friends, 1}).error().code == NetErrorCode::InvalidArgument);
    CHECK(host.services->createLobby({LobbyVisibility::Friends, 65}).error().code == NetErrorCode::InvalidArgument);
    const NetResult<RequestId> create = host.services->createLobby({LobbyVisibility::Public, 12});
    CHECK(bool(create));
    host.pump();
    const OnlineEvent* created = host.find(OnlineEventKind::LobbyCreated);
    CHECK(created && created->request == create.value() && created->lobby.valid());
    if (!created) return;
    const LobbyId lobby = created->lobby;
    CHECK(bool(host.services->setLobbyMetadata(lobby, "sw_game", "arena")));
    CHECK(bool(host.services->setLobbyMetadata(lobby, "sw_protocol", "1")));
    CHECK(!host.services->setLobbyMetadata(lobby, "", "x"));
    CHECK(!host.services->setLobbyMetadata(lobby, std::string(256, 'k'), "x"));
    CHECK(bool(host.services->setLobbyJoinable(lobby, true)));
    CHECK(!client.services->setLobbyMetadata(lobby, "sw_game", "stolen"));   // not the owner

    LobbyQuery query;
    query.requiredMetadata = {{"sw_game", "arena"}, {"sw_protocol", "1"}};
    const NetResult<RequestId> find = client.services->findLobbies(query);
    CHECK(bool(find));
    client.pump();
    const OnlineEvent* list = client.find(OnlineEventKind::LobbyList);
    CHECK(list && list->request == find.value() && list->lobbies.size() == 1);
    if (list && list->lobbies.size() == 1) {
        const LobbySummary& summary = list->lobbies[0];
        CHECK(summary.id == lobby && summary.owner == PeerId{kHostId});
        CHECK(summary.members == 1 && summary.capacity == 12);
        CHECK(summary.metadata.size() == 2);
    }
    query.requiredMetadata = {{"sw_game", "other"}};
    CHECK(bool(client.services->findLobbies(query)));
    client.events.clear();
    client.pump();
    CHECK(client.find(OnlineEventKind::LobbyList) && client.find(OnlineEventKind::LobbyList)->lobbies.empty());

    const NetResult<RequestId> join = client.services->joinLobby(lobby);
    CHECK(bool(join));
    client.pump();
    host.pump();
    const OnlineEvent* joined = client.find(OnlineEventKind::LobbyJoined);
    CHECK(joined && joined->request == join.value() && joined->lobby == lobby);
    CHECK(host.find(OnlineEventKind::LobbyMembersChanged) != nullptr);
    const NetResult<std::vector<PeerId>> members = host.services->lobbyMembers(lobby);
    CHECK(members && members->size() == 2);
    CHECK(bool(host.services->inviteFriend(lobby)) && host.mock->counts().invites == 1);
    CHECK(!client.services->joinLobby(LobbyId{}));

    CHECK(bool(client.services->leaveLobby(lobby)));
    client.pump();
    CHECK(client.find(OnlineEventKind::LobbyLeft) != nullptr);
    CHECK(host.services->lobbyMembers(lobby)->size() == 1);

    // A failed join reports RequestFailed for that request.
    client.events.clear();
    const NetResult<RequestId> missing = client.services->joinLobby(LobbyId{12345});
    client.pump();
    const OnlineEvent* failed = client.find(OnlineEventKind::RequestFailed);
    CHECK(failed && failed->request == missing.value());

    // Shutting down leaves the lobbies we're in.
    const uint32_t left = host.mock->counts().lobbiesLeft;
    host.services->shutdown();
    CHECK(host.mock->counts().lobbiesLeft == left + 1);
}

static void testLateLobbyResults() {
    MockSteamNetwork network;
    Node host(network, kHostId), client(network, kClientId);
    CHECK(host.start() && client.start());

    // Cancelled before Steam answers: the lobby it made is left, and nothing is reported.
    host.mock->faults().holdLobbyResults = true;
    const NetResult<RequestId> create = host.services->createLobby({});
    CHECK(bool(create));
    host.services->cancel(create.value());
    host.mock->releaseHeldResults();
    host.pump();
    CHECK(host.events.empty());
    CHECK(host.mock->counts().lobbiesLeft == 1);

    // Answered but not yet polled: still never reported.
    host.mock->faults().holdLobbyResults = false;
    const NetResult<RequestId> second = host.services->createLobby({});
    host.services->framePass();
    host.services->cancel(second.value());
    host.pump();
    CHECK(host.events.empty());

    // A join cancelled before Steam answers is left too.
    const NetResult<RequestId> third = host.services->createLobby({});
    host.pump();
    const OnlineEvent* created = host.find(OnlineEventKind::LobbyCreated);
    CHECK(created && created->request == third.value());
    if (!created) return;
    client.mock->faults().holdLobbyResults = true;
    const NetResult<RequestId> join = client.services->joinLobby(created->lobby);
    client.services->cancel(join.value());
    client.mock->releaseHeldResults();
    client.pump();
    CHECK(client.events.empty());
    CHECK(client.mock->counts().lobbiesLeft == 1);

    // A result from before a restart is ignored.
    const NetResult<RequestId> stale = client.services->findLobbies({});
    client.services->shutdown();
    CHECK(client.start());
    client.pump();
    CHECK(client.events.empty());
    client.services->cancel(stale.value());   // an old generation's request: harmless
}

static std::vector<uint8_t> ticketFor(Node& from, PeerId to) {
    const NetResult<RequestId> request = from.services->requestAuthTicket(to);
    CHECK(bool(request));
    if (!request) return {};
    CHECK(from.services->authTicket(request.value()).error().code == NetErrorCode::InvalidState);   // not ready
    from.pump();
    const OnlineEvent* ready = nullptr;
    for (const OnlineEvent& e : from.events)
        if (e.kind == OnlineEventKind::AuthTicketReady && e.request == request.value()) ready = &e;
    CHECK(ready && ready->peer == to);
    const NetResult<std::span<const uint8_t>> bytes = from.services->authTicket(request.value());
    CHECK(bool(bytes));
    if (!bytes) return {};
    return {bytes->begin(), bytes->end()};
}

static void testAuth() {
    MockSteamNetwork network;
    Node host(network, kHostId), client(network, kClientId);
    CHECK(host.start() && client.start());
    const PeerId hostPeer{kHostId}, clientPeer{kClientId};

    // Both ways: the host validates the client and the client validates the host.
    const std::vector<uint8_t> clientTicket = ticketFor(client, hostPeer);
    const std::vector<uint8_t> hostTicket = ticketFor(host, clientPeer);
    CHECK(!clientTicket.empty() && clientTicket.size() <= 4096);
    CHECK(bool(host.services->beginAuthSession(clientPeer, clientTicket)));
    CHECK(bool(client.services->beginAuthSession(hostPeer, hostTicket)));
    CHECK(host.services->beginAuthSession(clientPeer, clientTicket).error().code == NetErrorCode::InvalidState);
    host.pump();
    client.pump();
    const OnlineEvent* ok = host.find(OnlineEventKind::AuthSucceeded);
    CHECK(ok && ok->peer == clientPeer);
    CHECK(client.find(OnlineEventKind::AuthSucceeded) && client.find(OnlineEventKind::AuthSucceeded)->peer == hostPeer);
    CHECK(host.count(OnlineEventKind::AuthFailed) == 0);

    // Steam repeating the success is reported once; a later revocation fails the peer.
    host.mock->injectValidation(kClientId, SteamAuthResponse::Ok);
    host.pump();
    CHECK(host.count(OnlineEventKind::AuthSucceeded) == 1);
    host.mock->injectValidation(kClientId, SteamAuthResponse::VacBanned);
    host.pump();
    const OnlineEvent* revoked = host.find(OnlineEventKind::AuthFailed);
    CHECK(revoked && revoked->peer == clientPeer && revoked->reason == ReasonCode::AuthenticationFailed);
    CHECK(host.mock->counts().authSessionsEnded == 1);
    // ...and anything Steam says about that session afterwards is ignored.
    host.events.clear();
    host.mock->injectValidation(kClientId, SteamAuthResponse::Ok);
    host.pump();
    CHECK(host.events.empty());

    // Sizes and bad input.
    CHECK(host.services->beginAuthSession(clientPeer, {}).error().code == NetErrorCode::InvalidArgument);
    const std::vector<uint8_t> huge(4097, 1);
    CHECK(host.services->beginAuthSession(clientPeer, huge).error().code == NetErrorCode::InvalidArgument);
    CHECK(host.services->beginAuthSession(PeerId{}, clientTicket).error().code == NetErrorCode::InvalidArgument);
    CHECK(!host.services->requestAuthTicket(PeerId{}));
    host.mock->faults().beginAuth = SteamBeginAuth::InvalidTicket;
    CHECK(host.services->beginAuthSession(clientPeer, clientTicket).error().code == NetErrorCode::InvalidArgument);
    host.mock->faults().beginAuth = SteamBeginAuth::Ok;

    // A ticket made for someone else fails.
    Node other(network, 76561197960287999ull);
    CHECK(other.start());
    const std::vector<uint8_t> wrong = ticketFor(other, PeerId{12345});
    CHECK(bool(host.services->beginAuthSession(PeerId{76561197960287999ull}, wrong)));
    host.events.clear();
    host.pump();
    const OnlineEvent* rejected = host.find(OnlineEventKind::AuthFailed);
    CHECK(rejected && rejected->reason == ReasonCode::AuthenticationFailed);

    // Cancelling a ticket after it was validated fails the session that used it.
    host.services->endAuthSession(clientPeer);
    const NetResult<RequestId> fresh = client.services->requestAuthTicket(hostPeer);
    client.pump();
    const std::span<const uint8_t> freshBytes = client.services->authTicket(fresh.value()).value();
    CHECK(bool(host.services->beginAuthSession(clientPeer, freshBytes)));
    host.events.clear();
    host.pump();
    CHECK(host.find(OnlineEventKind::AuthSucceeded) != nullptr);
    client.services->cancel(fresh.value());
    CHECK(!client.services->authTicket(fresh.value()));
    host.pump();
    const OnlineEvent* cancelled = host.find(OnlineEventKind::AuthFailed);
    CHECK(cancelled && cancelled->peer == clientPeer && cancelled->reason == ReasonCode::AuthenticationFailed);

    // No answer from Steam: fail closed with AuthUnavailable after 15 seconds, not before.
    host.services->endAuthSession(clientPeer);
    host.mock->faults().validation.reset();
    const std::vector<uint8_t> slow = ticketFor(client, hostPeer);
    CHECK(bool(host.services->beginAuthSession(clientPeer, slow)));
    host.events.clear();
    network.advance(14'900'000);
    host.pump();
    CHECK(host.events.empty());
    network.advance(200'000);
    host.pump();
    const OnlineEvent* timedOut = host.find(OnlineEventKind::AuthFailed);
    CHECK(timedOut && timedOut->peer == clientPeer && timedOut->reason == ReasonCode::AuthUnavailable);

    // Steam unable to decide is AuthUnavailable too, not a bad ticket.
    host.mock->faults().validation = SteamAuthResponse::UserNotConnectedToSteam;
    const std::vector<uint8_t> offline = ticketFor(client, hostPeer);
    CHECK(bool(host.services->beginAuthSession(clientPeer, offline)));
    host.events.clear();
    host.pump();
    CHECK(
        host.find(OnlineEventKind::AuthFailed) &&
        host.find(OnlineEventKind::AuthFailed)->reason == ReasonCode::AuthUnavailable);

    // A ticket Steam couldn't make is a failed request.
    client.mock->faults().failTickets = true;
    const NetResult<RequestId> broken = client.services->requestAuthTicket(hostPeer);
    client.events.clear();
    client.pump();
    const OnlineEvent* brokenEvent = client.find(OnlineEventKind::RequestFailed);
    CHECK(brokenEvent && brokenEvent->request == broken.value());
    client.mock->faults().failTickets = false;

    // endAuthSession cancels the tickets we gave that peer; shutdown cancels the rest and
    // ends every session.
    client.mock->faults().holdTickets = true;
    CHECK(bool(client.services->requestAuthTicket(hostPeer)));
    CHECK(bool(client.services->requestAuthTicket(PeerId{777})));
    const uint32_t cancelledBefore = client.mock->counts().ticketsCancelled;
    client.services->endAuthSession(hostPeer);
    CHECK(client.mock->counts().ticketsCancelled > cancelledBefore);
    CHECK(client.mock->counts().ticketsIssued == client.mock->counts().ticketsCancelled + 1);   // 777's
    host.mock->faults().validation.reset();
    const std::vector<uint8_t> pending = ticketFor(other, hostPeer);
    CHECK(bool(host.services->beginAuthSession(PeerId{76561197960287999ull}, pending)));
    client.services->shutdown();
    host.services->shutdown();
    for (Node* node : {&client, &host}) {
        CHECK(node->mock->counts().ticketsIssued == node->mock->counts().ticketsCancelled);
        CHECK(node->mock->counts().authSessionsBegun == node->mock->counts().authSessionsEnded);
    }
}

int main() {
    testNoSdkFactory();
    testLifecycle();
    testDedicatedServer();
    testLobbies();
    testLateLobbyResults();
    testAuth();
    if (failures) {
        std::fprintf(stderr, "test_steam_services: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_steam_services: all checks passed\n");
    return 0;
}
