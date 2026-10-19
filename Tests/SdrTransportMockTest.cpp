// SteamNetworkAdapter (SteamServices/SteamNetworkAdapter.h), the Steam Datagram Relay
// transport, against MockSteamApi: the connection state table (Incoming, accept, refuse
// when full or too late, our own Connecting ignored, Connected and Closed once, stale and
// foreign handles), listen and poll-group failures and their cleanup, send flags, sizes
// and results, the 32 KiB unreliable backpressure, receive budgets with a ring buffer,
// every Steam message released exactly once (oversize, unknown connection, a throwing
// sink), broadcast, RTT, shutdown with transports alive, and no allocation across a
// continuous 10,000-message poll loop.
#include <SteamServices/MockSteamApi.h>
#include <SteamServices/SteamServices.h>

#include <Network/Messages.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
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

static constexpr uint64_t kHostId = 76561197960287930ull;
static constexpr uint16_t kPort = 7;
static const TransportAddress kHost{TransportAddress::SteamPeer, kHostId, kPort};

struct Received {
    ConnectionId connection;
    PeerId peer;
    Delivery delivery;
    uint32_t sequence;
    uint32_t size;
    bool intact;
};

// Records into storage reserved up front, so polling allocates nothing.
struct Recorder : TransportSink {
    std::vector<TransportEvent> events;
    std::vector<Received> messages;
    ITransport* acceptFrom = nullptr;
    bool reject = false;
    bool throwOnMessage = false;

    Recorder() {
        events.reserve(4096);
        messages.reserve(16384);
    }
    void onEvent(const TransportEvent& event) override {
        if (events.size() < events.capacity()) events.push_back(event);
        if (event.kind == TransportEventKind::Incoming && acceptFrom) {
            if (reject)
                acceptFrom->close(event.connection, ReasonCode::NotLobbyMember);
            else
                CHECK(bool(acceptFrom->accept(event.connection)));
        }
    }
    void onMessage(const ReceivedMessage& m) override {
        if (throwOnMessage) throw std::runtime_error("sink failed");
        uint32_t sequence = 0;
        if (m.bytes.size() >= 4) std::memcpy(&sequence, m.bytes.data(), 4);
        // Every byte after the sequence is (sequence + index) & 0xFF.
        bool intact = true;
        for (size_t i = 4; i < m.bytes.size(); ++i) intact &= m.bytes[i] == uint8_t(sequence + i);
        if (messages.size() < messages.capacity())
            messages.push_back({m.connection, m.peer, m.delivery, sequence, uint32_t(m.bytes.size()), intact});
    }
    size_t count(TransportEventKind kind) const {
        return size_t(
            std::count_if(events.begin(), events.end(), [&](const TransportEvent& e) { return e.kind == kind; }));
    }
    const TransportEvent* last(TransportEventKind kind) const {
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (it->kind == kind) return &*it;
        return nullptr;
    }
    void clear() {
        events.clear();
        messages.clear();
    }
};

static void fill(std::vector<uint8_t>& buffer, uint32_t sequence, size_t size) {
    buffer.resize(size);
    std::memcpy(buffer.data(), &sequence, 4);
    for (size_t i = 4; i < size; ++i) buffer[i] = uint8_t(sequence + i);
}

struct Peer {
    MockSteamApi* mock = nullptr;
    std::unique_ptr<SteamServices> services;
    std::unique_ptr<SteamNetworkAdapter> transport;
    Recorder sink;

    Peer(MockSteamNetwork& network, uint64_t steamId, SteamServicesSettings settings = {}) {
        std::unique_ptr<MockSteamApi> api = network.makeApi(steamId);
        mock = api.get();
        services = std::make_unique<SteamServices>(std::move(api));
        if (!settings.appId) {
            settings.appId = 480;
            settings.virtualPort = kPort;
        }
        CHECK(bool(services->initializeClient(settings)));
        NetResult<std::unique_ptr<SteamNetworkAdapter>> made = services->createSteamTransport();
        CHECK(bool(made));
        if (made) transport = std::move(made.value());
    }
    PollResult pump(const ReceiveBudget& budget = {}) {
        services->framePass();
        return transport->poll(sink, budget);
    }
};

// A host and `clients` clients, all connected.
struct Session {
    MockSteamNetwork network;
    Peer host;
    std::vector<std::unique_ptr<Peer>> clients;
    std::vector<ConnectionId> toHost, toClient;

    explicit Session(uint32_t clientCount = 1, TransportLimits limits = {}, MockSteamConfig config = {})
        : network(config), host(network, kHostId) {
        CHECK(bool(host.transport->listen(kHost, limits)));
        host.sink.acceptFrom = host.transport.get();
        for (uint32_t i = 0; i < clientCount; ++i) {
            clients.push_back(std::make_unique<Peer>(network, kHostId + 1 + i));
            NetResult<ConnectionId> id = clients.back()->transport->connect(kHost);
            CHECK(bool(id));
            toHost.push_back(id ? id.value() : ConnectionId{});
        }
        pumpAll();
        pumpAll();
        for (uint32_t i = 0; i < clientCount; ++i) {
            CHECK(clients[i]->sink.count(TransportEventKind::Connected) == 1);
            toClient.push_back({});
            for (const TransportEvent& e : host.sink.events)
                if (e.kind == TransportEventKind::Connected && e.peer == PeerId{kHostId + 1 + i})
                    toClient.back() = e.connection;
        }
    }
    void pumpAll(const ReceiveBudget& budget = {}) {
        host.pump(budget);
        for (auto& c : clients) c->pump(budget);
    }
    Peer& client(size_t i = 0) { return *clients[i]; }
};

static void testConnect() {
    Session s(1);
    Peer& client = s.client();
    CHECK(s.host.sink.count(TransportEventKind::Incoming) == 1);
    CHECK(s.host.sink.count(TransportEventKind::Connected) == 1);
    const TransportEvent* incoming = s.host.sink.last(TransportEventKind::Incoming);
    CHECK(incoming && incoming->peer == PeerId{kHostId + 1});   // the identity Steam authenticated
    const TransportEvent* connected = client.sink.last(TransportEventKind::Connected);
    CHECK(connected && connected->peer == PeerId{kHostId} && connected->connection == s.toHost[0]);
    CHECK(connected && connected->endpointGeneration == client.transport->endpointGeneration());
    // Our own connect's Connecting callback is ignored, not accepted.
    CHECK(client.transport->counters().ignoredStateChanges >= 1);
    CHECK(client.transport->localPeer() == PeerId{kHostId + 1});

    // Connected once, even if Steam says so again.
    const uint32_t handle = client.mock->connectionTo(kHostId);
    client.mock->injectConnectionChange(
        {handle, 0, SteamConnectionState::Connected, SteamConnectionState::Connected, kHostId, 0});
    s.pumpAll();
    CHECK(client.sink.count(TransportEventKind::Connected) == 1);

    // RTT from Steam's real-time status.
    const std::optional<ConnectionStats> stats = client.transport->stats(s.toHost[0]);
    CHECK(
        stats && stats->state == ConnectionState::Connected && stats->rttUs == 25000 && stats->peer == PeerId{kHostId});

    // Bad calls.
    CHECK(
        client.transport->connect({TransportAddress::Loopback, kHostId, kPort}).error().code ==
        NetErrorCode::InvalidArgument);
    CHECK(
        client.transport->connect({TransportAddress::SteamPeer, 0, kPort}).error().code ==
        NetErrorCode::InvalidArgument);
    CHECK(
        client.transport->connect({TransportAddress::SteamPeer, kHostId + 1, kPort}).error().code ==
        NetErrorCode::InvalidArgument);   // ourselves
    CHECK(s.host.transport->listen(kHost, {}).error().code == NetErrorCode::InvalidState);
    CHECK(s.host.transport->accept(s.toClient[0]).error().code == NetErrorCode::InvalidState);
    CHECK(s.host.transport->accept({60, 99}).error().code == NetErrorCode::NotConnected);
}

static void testListenFailures() {
    MockSteamNetwork network;
    Peer host(network, kHostId);
    CHECK(
        host.transport->listen({TransportAddress::Loopback, 1, kPort}, {}).error().code ==
        NetErrorCode::InvalidArgument);
    CHECK(host.transport->listen(kHost, {0, 16384}).error().code == NetErrorCode::InvalidArgument);
    CHECK(host.transport->listen(kHost, {69, 16384}).error().code == NetErrorCode::InvalidArgument);
    CHECK(host.transport->listen(kHost, {11, 16385}).error().code == NetErrorCode::InvalidArgument);

    host.mock->faults().failPollGroup = true;
    const uint32_t generation = host.transport->endpointGeneration();
    CHECK(host.transport->listen(kHost, {}).error().code == NetErrorCode::Io);
    CHECK(host.transport->counters().pollGroupFailures == 1);
    CHECK(host.mock->counts().listenSockets == 0);
    host.mock->faults().failPollGroup = false;

    // A listen socket that fails leaves no poll group behind.
    host.mock->faults().failListen = true;
    CHECK(host.transport->listen(kHost, {}).error().code == NetErrorCode::AddressInUse);
    CHECK(host.mock->counts().pollGroups == 0 && host.mock->counts().listenSockets == 0);
    CHECK(host.transport->endpointGeneration() == generation);
    host.mock->faults().failListen = false;

    CHECK(bool(host.transport->listen(kHost, {})));
    CHECK(host.transport->endpointGeneration() == generation + 1);
    CHECK(host.mock->counts().pollGroups == 1 && host.mock->counts().listenSockets == 1);
    host.transport->closeAll(ReasonCode::Normal);
    CHECK(host.transport->endpointGeneration() == generation + 2);
    CHECK(host.mock->counts().listenSockets == 0);
    CHECK(bool(host.transport->listen(kHost, {})));   // and again

    // Connect failures.
    Peer client(network, kHostId + 1);
    client.mock->faults().failPollGroup = true;
    CHECK(client.transport->connect(kHost).error().code == NetErrorCode::Io);
    client.mock->faults().failPollGroup = false;
    client.mock->faults().failConnect = true;
    CHECK(client.transport->connect(kHost).error().code == NetErrorCode::Io);
    CHECK(client.mock->counts().openConnections == 0);
}

static void testRefusals() {
    // Nobody listening there: ProblemDetectedLocally.
    {
        MockSteamNetwork network;
        Peer client(network, kHostId + 1);
        NetResult<ConnectionId> id = client.transport->connect(kHost);
        CHECK(bool(id));
        client.pump();
        const TransportEvent* closed = client.sink.last(TransportEventKind::Closed);
        CHECK(closed && closed->cause == CloseCause::ProblemDetectedLocally && closed->connection == id.value());
        CHECK(!client.transport->stats(id.value()));
        CHECK(client.mock->counts().openConnections == 0);   // the adapter closed Steam's handle too
    }
    // Rejected by the host: Rejected, with the host's reason.
    {
        MockSteamNetwork network;
        Peer host(network, kHostId), client(network, kHostId + 1);
        CHECK(bool(host.transport->listen(kHost, {})));
        host.sink.acceptFrom = host.transport.get();
        host.sink.reject = true;
        CHECK(bool(client.transport->connect(kHost)));
        for (int i = 0; i < 3; ++i) {
            host.pump();
            client.pump();
        }
        const TransportEvent* closed = client.sink.last(TransportEventKind::Closed);
        CHECK(closed && closed->cause == CloseCause::Rejected && closed->reason == ReasonCode::NotLobbyMember);
        CHECK(host.mock->counts().openConnections == 0 && client.mock->counts().openConnections == 0);
    }
    // Not accepted by the next poll: refused.
    {
        MockSteamNetwork network;
        Peer host(network, kHostId), client(network, kHostId + 1);
        CHECK(bool(host.transport->listen(kHost, {})));
        CHECK(bool(client.transport->connect(kHost)));
        host.pump();
        CHECK(host.sink.count(TransportEventKind::Incoming) == 1);
        host.pump();
        client.pump();
        CHECK(host.transport->counters().refusedIncoming == 1);
        const TransportEvent* closed = client.sink.last(TransportEventKind::Closed);
        CHECK(closed && closed->cause == CloseCause::Rejected && closed->reason == ReasonCode::Timeout);
    }
    // Full: refused before anyone hears of it, with ServerFull.
    {
        Session s(1, {1, 16384});
        Peer late(s.network, kHostId + 50);
        CHECK(bool(late.transport->connect(kHost)));
        s.pumpAll();
        late.pump();
        CHECK(s.host.sink.count(TransportEventKind::Incoming) == 1);
        CHECK(s.host.transport->counters().refusedIncoming == 1);
        const TransportEvent* closed = late.sink.last(TransportEventKind::Closed);
        CHECK(closed && closed->cause == CloseCause::Rejected && closed->reason == ReasonCode::ServerFull);
    }
    // A poll group Steam won't take: the connection closes as a local problem, both ends.
    {
        MockSteamNetwork network;
        Peer host(network, kHostId), client(network, kHostId + 1);
        CHECK(bool(host.transport->listen(kHost, {})));
        host.sink.acceptFrom = host.transport.get();
        host.mock->faults().failSetPollGroup = true;
        CHECK(bool(client.transport->connect(kHost)));
        for (int i = 0; i < 4; ++i) {
            host.pump();
            client.pump();
        }
        const TransportEvent* closed = host.sink.last(TransportEventKind::Closed);
        CHECK(closed && closed->cause == CloseCause::ProblemDetectedLocally);
        CHECK(host.sink.count(TransportEventKind::Connected) == 0);
        CHECK(host.transport->counters().pollGroupFailures == 1);
        CHECK(client.sink.last(TransportEventKind::Closed) != nullptr);
        CHECK(host.mock->counts().openConnections == 0 && client.mock->counts().openConnections == 0);
    }
    // Steam won't accept.
    {
        MockSteamNetwork network;
        Peer host(network, kHostId), client(network, kHostId + 1);
        CHECK(bool(host.transport->listen(kHost, {})));
        CHECK(bool(client.transport->connect(kHost)));
        host.pump();
        const TransportEvent* incoming = host.sink.last(TransportEventKind::Incoming);
        CHECK(incoming != nullptr);
        host.mock->faults().failAccept = true;
        if (incoming) CHECK(host.transport->accept(incoming->connection).error().code == NetErrorCode::Io);
        CHECK(host.mock->counts().openConnections == 0);
    }
}

static void testMessages() {
    Session s(1);
    Peer& client = s.client();
    std::vector<uint8_t> buffer;
    fill(buffer, 1, 100);
    CHECK(client.transport->send(s.toHost[0], buffer, Delivery::ReliableOrdered) == SendResult::Queued);
    fill(buffer, 2, kMaxUnreliableMessageBytes);
    CHECK(client.transport->send(s.toHost[0], buffer, Delivery::Unreliable) == SendResult::Queued);
    fill(buffer, 3, kMaxUnreliableMessageBytes + 1);
    CHECK(client.transport->send(s.toHost[0], buffer, Delivery::Unreliable) == SendResult::Failed);
    fill(buffer, 4, kMaxReliableMessageBytes);
    CHECK(client.transport->send(s.toHost[0], buffer, Delivery::ReliableOrdered) == SendResult::Queued);
    fill(buffer, 5, kMaxReliableMessageBytes + 1);
    CHECK(client.transport->send(s.toHost[0], buffer, Delivery::ReliableOrdered) == SendResult::Failed);
    CHECK(client.transport->send(s.toHost[0], {}, Delivery::ReliableOrdered) == SendResult::Failed);
    CHECK(client.transport->send({3, 77}, buffer, Delivery::ReliableOrdered) == SendResult::NotConnected);
    s.pumpAll();
    CHECK(s.host.sink.messages.size() == 3);
    if (s.host.sink.messages.size() == 3) {
        const Received& a = s.host.sink.messages[0];
        CHECK(a.sequence == 1 && a.size == 100 && a.delivery == Delivery::ReliableOrdered && a.intact);
        CHECK(a.peer == PeerId{kHostId + 1} && a.connection == s.toClient[0]);
        CHECK(s.host.sink.messages[1].delivery == Delivery::Unreliable && s.host.sink.messages[1].intact);
        CHECK(s.host.sink.messages[2].size == kMaxReliableMessageBytes && s.host.sink.messages[2].intact);
    }
    // The flags Steam was asked for are ReliableNoNagle and UnreliableNoNagle (checked in
    // the mock by what each message arrived as, above).
    const std::optional<ConnectionStats> stats = s.host.transport->stats(s.toClient[0]);
    CHECK(stats && stats->messagesReceived == 3 && stats->bytesReceived == 100 + 1152 + 16384);
    CHECK(client.mock->counts().outstandingMessages == 0 && s.host.mock->counts().outstandingMessages == 0);

    // The listener's own limit applies to what it sends and receives.
    Session small(1, {11, 2000});
    fill(buffer, 9, 2001);
    CHECK(small.host.transport->send(small.toClient[0], buffer, Delivery::ReliableOrdered) == SendResult::Failed);
    CHECK(small.client().transport->send(small.toHost[0], buffer, Delivery::ReliableOrdered) == SendResult::Queued);
    small.pumpAll();
    CHECK(small.host.sink.messages.empty() && small.host.transport->counters().droppedOversize == 1);

    // max_snapshot_bytes from the settings caps unreliable messages.
    MockSteamNetwork network;
    SteamServicesSettings tight;
    tight.appId = 480;
    tight.virtualPort = kPort;
    tight.maxUnreliableBytes = 600;
    Peer host(network, kHostId, tight), sender(network, kHostId + 1);
    CHECK(bool(host.transport->listen(kHost, {})));
    host.sink.acceptFrom = host.transport.get();
    NetResult<ConnectionId> id = sender.transport->connect(kHost);
    for (int i = 0; i < 3; ++i) {
        host.pump();
        sender.pump();
    }
    const TransportEvent* toSender = host.sink.last(TransportEventKind::Connected);
    CHECK(toSender != nullptr);
    if (!toSender || !id) return;
    fill(buffer, 10, 601);
    CHECK(host.transport->send(toSender->connection, buffer, Delivery::Unreliable) == SendResult::Failed);
    CHECK(
        sender.transport->send(id.value(), buffer, Delivery::Unreliable) == SendResult::Queued);   // 1152 allowed there
    host.pump();
    CHECK(host.sink.messages.empty() && host.transport->counters().droppedOversize == 1);   // too big for this end
}

static void testSendResults() {
    Session s(1);
    Peer& client = s.client();
    std::vector<uint8_t> buffer;
    fill(buffer, 1, 200);

    // More than 32 KiB waiting on the connection: unreliable state is dropped, reliable still goes.
    client.mock->faults().pendingReliableBytes = 20000;
    client.mock->faults().pendingUnreliableBytes = 13000;
    client.pump();   // re-reads Steam's queue
    CHECK(client.transport->send(s.toHost[0], buffer, Delivery::Unreliable) == SendResult::Dropped);
    CHECK(client.transport->counters().unreliableBackpressure == 1);
    CHECK(client.transport->send(s.toHost[0], buffer, Delivery::ReliableOrdered) == SendResult::Queued);
    const std::optional<ConnectionStats> stats = client.transport->stats(s.toHost[0]);
    CHECK(stats && stats->pendingReliableBytes == 20200 && stats->pendingUnreliableBytes == 13000);
    client.mock->faults().pendingReliableBytes = client.mock->faults().pendingUnreliableBytes = 0;
    client.pump();
    CHECK(client.transport->send(s.toHost[0], buffer, Delivery::Unreliable) == SendResult::Queued);

    // Steam's own send results.
    struct Case {
        SteamResult steam;
        Delivery delivery;
        SendResult expected;
    };
    const Case cases[] = {
        {SteamResult::LimitExceeded, Delivery::ReliableOrdered, SendResult::Backpressure},
        {SteamResult::LimitExceeded, Delivery::Unreliable, SendResult::Dropped},
        {SteamResult::NoConnection, Delivery::ReliableOrdered, SendResult::NotConnected},
        {SteamResult::InvalidState, Delivery::Unreliable, SendResult::NotConnected},
        {SteamResult::InvalidParam, Delivery::ReliableOrdered, SendResult::Failed},
        {SteamResult::Fail, Delivery::Unreliable, SendResult::Failed},
    };
    for (const Case& c : cases) {
        client.mock->faults().sendResult = c.steam;
        CHECK(client.transport->send(s.toHost[0], buffer, c.delivery) == c.expected);
    }
    CHECK(client.transport->counters().sendFailures == 6);
    client.mock->faults().sendResult.reset();
    CHECK(client.transport->send(s.toHost[0], buffer, Delivery::ReliableOrdered) == SendResult::Queued);
}

static void testReleaseAndBudgets() {
    Session s(1);
    Peer& client = s.client();
    std::vector<uint8_t> buffer;
    const uint32_t raw = client.mock->connectionTo(kHostId);

    // Messages the adapter refuses are still released: empty, oversize, after a close.
    CHECK(client.mock->sendMessage(raw, {}, SteamSend::ReliableNoNagle) == SteamResult::Ok);
    fill(buffer, 1, kMaxUnreliableMessageBytes + 1);
    CHECK(client.mock->sendMessage(raw, buffer, SteamSend::UnreliableNoNagle) == SteamResult::Ok);
    s.host.pump();
    CHECK(s.host.sink.messages.empty());
    CHECK(s.host.transport->counters().droppedOversize == 2);
    CHECK(s.host.mock->counts().outstandingMessages == 0);
    CHECK(s.host.mock->counts().releasedMessages == s.host.mock->counts().receivedMessages);

    // A sink that throws: everything Steam handed over is released anyway.
    for (uint32_t i = 0; i < 5; ++i) {
        fill(buffer, i, 64);
        CHECK(client.transport->send(s.toHost[0], buffer, Delivery::Unreliable) == SendResult::Queued);
    }
    s.host.sink.throwOnMessage = true;
    bool threw = false;
    try {
        s.host.pump();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(s.host.mock->counts().outstandingMessages == 0 && s.host.mock->counts().badReleases == 0);
    s.host.sink.throwOnMessage = false;
    s.host.pump();   // the rest, from the ring
    CHECK(s.host.sink.messages.size() == 4);

    // Budgets: what doesn't fit waits for the next poll, in order, copied out of Steam.
    s.host.sink.clear();
    for (uint32_t i = 0; i < 100; ++i) {
        fill(buffer, 1000 + i, 500);
        CHECK(client.transport->send(s.toHost[0], buffer, Delivery::ReliableOrdered) == SendResult::Queued);
    }
    PollResult result = s.host.pump({10, 262144});
    CHECK(result.messages == 10 && result.budgetExhausted);
    CHECK(s.host.mock->counts().outstandingMessages == 0);   // the batch was copied and released
    result = s.host.pump({256, 2000});
    CHECK(result.messages == 4 && result.bytes == 2000 && result.budgetExhausted);
    result = s.host.pump({1, 1});   // a message larger than the byte budget still goes, alone
    CHECK(result.messages == 1 && result.budgetExhausted);
    while (s.host.pump().messages) {}
    CHECK(s.host.sink.messages.size() == 100);
    bool ordered = true;
    for (size_t i = 0; i < s.host.sink.messages.size(); ++i)
        ordered &= s.host.sink.messages[i].sequence == 1000 + i && s.host.sink.messages[i].intact;
    CHECK(ordered);

    // Messages waiting in the ring for a connection that closes are dropped, not delivered.
    client.pump();   // the 50 KB of reliable sends above count as queued until the client re-reads Steam
    for (uint32_t i = 0; i < 20; ++i) {
        fill(buffer, i, 64);
        CHECK(client.transport->send(s.toHost[0], buffer, Delivery::Unreliable) == SendResult::Queued);
    }
    s.host.sink.clear();
    s.host.pump({5, 262144});
    s.host.transport->close(s.toClient[0], ReasonCode::Normal);
    s.host.pump();
    CHECK(s.host.sink.messages.size() == 5);
    CHECK(s.host.mock->counts().outstandingMessages == 0 && s.host.mock->counts().badReleases == 0);
    CHECK(s.host.mock->counts().releasedMessages == s.host.mock->counts().receivedMessages);
}

static void testCloseAndStale() {
    Session s(2);
    Peer& a = s.client(0);
    Peer& b = s.client(1);
    s.host.transport->close(s.toClient[0], ReasonCode::HostLeft);
    CHECK(!s.host.transport->stats(s.toClient[0]));
    std::vector<uint8_t> buffer;
    fill(buffer, 1, 32);
    CHECK(s.host.transport->send(s.toClient[0], buffer, Delivery::ReliableOrdered) == SendResult::NotConnected);
    s.pumpAll();
    const TransportEvent* closed = a.sink.last(TransportEventKind::Closed);
    CHECK(closed && closed->cause == CloseCause::ClosedByPeer && closed->reason == ReasonCode::HostLeft);
    CHECK(s.host.sink.count(TransportEventKind::Closed) == 0);   // we closed it: no event for us
    CHECK(a.mock->counts().openConnections == 0);

    // The peer closes: Closed once, with its reason.
    b.transport->close(s.toHost[1], ReasonCode::Timeout);
    s.pumpAll();
    s.pumpAll();
    CHECK(s.host.sink.count(TransportEventKind::Closed) == 1);
    const TransportEvent* gone = s.host.sink.last(TransportEventKind::Closed);
    CHECK(
        gone && gone->cause == CloseCause::ClosedByPeer && gone->reason == ReasonCode::Timeout &&
        gone->connection == s.toClient[1]);
    CHECK(s.host.mock->counts().openConnections == 0);

    // Stale and foreign handles are ignored.
    const uint64_t ignored = s.host.transport->counters().ignoredStateChanges;
    s.host.mock->injectConnectionChange(
        {9999, 0, SteamConnectionState::Connected, SteamConnectionState::Connecting, 5, 0});
    s.host.mock->injectConnectionChange(
        {9998, 0, SteamConnectionState::ClosedByPeer, SteamConnectionState::Connected, 5, 0});
    s.host.mock->injectConnectionChange(
        {9997, 4242, SteamConnectionState::Connecting, SteamConnectionState::None, 5, 0});
    s.host.sink.clear();
    s.pumpAll();
    CHECK(s.host.sink.events.empty());
    CHECK(s.host.transport->counters().ignoredStateChanges == ignored);   // nobody owned them

    // A Connecting from before closeAll() and a new listen() is ignored too.
    Peer c(s.network, kHostId + 60);
    CHECK(bool(c.transport->connect(kHost)));
    s.host.transport->closeAll(ReasonCode::Normal);
    CHECK(bool(s.host.transport->listen(kHost, {})));
    s.pumpAll();
    c.pump();
    CHECK(s.host.sink.count(TransportEventKind::Incoming) == 0);
    CHECK(c.sink.last(TransportEventKind::Closed) != nullptr);
}

static void testOverflowReconciles() {
    Session s(1);
    Peer& client = s.client();
    const uint32_t raw = s.host.mock->connectionTo(kHostId + 1);
    // More changes than the queue holds, for a connection the host owns.
    for (uint32_t i = 0; i < 4 * kMaxTransportConnections + 10; ++i)   // the queue holds 4 per connection
        s.host.mock->injectConnectionChange(
            {raw, 0, SteamConnectionState::Connected, SteamConnectionState::Connected, kHostId + 1, 0});
    client.transport->close(s.toHost[0], ReasonCode::Normal);   // and the real close is lost in the flood
    s.host.pump();
    s.host.pump();
    CHECK(s.host.transport->counters().stateChangeOverflows > 0);
    CHECK(s.host.sink.count(TransportEventKind::Closed) == 1);   // found by re-reading Steam's state
    CHECK(s.host.mock->counts().openConnections == 0);
}

static void testBroadcast() {
    Session s(3);
    std::vector<uint8_t> buffer;
    fill(buffer, 77, 300);
    const BroadcastResult all = s.host.transport->broadcast(buffer, Delivery::Unreliable);
    CHECK(all.queued == 3 && all.failed == 0);
    const BroadcastResult some = s.host.transport->broadcast(buffer, Delivery::ReliableOrdered, s.toClient[1]);
    CHECK(some.queued == 2);
    s.pumpAll();
    CHECK(
        s.client(0).sink.messages.size() == 2 && s.client(1).sink.messages.size() == 1 &&
        s.client(2).sink.messages.size() == 2);
}

static void testShutdownWithTransports() {
    Session s(2);
    Peer& client = s.client(0);
    s.host.services->shutdown();
    CHECK(!s.host.transport->attached());
    CHECK(
        s.host.mock->counts().openConnections == 0 && s.host.mock->counts().listenSockets == 0 &&
        s.host.mock->counts().pollGroups == 0);
    std::vector<uint8_t> buffer;
    fill(buffer, 1, 32);
    CHECK(s.host.transport->send(s.toClient[0], buffer, Delivery::ReliableOrdered) == SendResult::NotConnected);
    CHECK(s.host.transport->poll(s.host.sink, {}).messages == 0);
    CHECK(s.host.transport->listen(kHost, {}).error().code == NetErrorCode::Unavailable);
    CHECK(!s.host.services->createTransport());
    client.pump();
    const TransportEvent* closed = client.sink.last(TransportEventKind::Closed);
    CHECK(closed && closed->reason == ReasonCode::ServiceUnavailable);
    // The service can go before its transports.
    s.client(1).services.reset();
    CHECK(!s.client(1).transport->attached());
    s.client(1).transport.reset();
}

static void testNoAllocationPollLoop() {
    // 4 clients each send to the host; the host polls every frame and echoes every tenth
    // message back reliably. Counted from the first send to the last poll.
    MockSteamConfig config;
    config.messagePool = 4096;
    config.messageBytes = kMaxReliableMessageBytes;
    Session s(4, {11, kMaxReliableMessageBytes}, config);
    std::vector<uint8_t> buffer(kMaxUnreliableMessageBytes);
    constexpr uint32_t kMessages = 10000;
    s.host.sink.clear();
    const uint64_t before = g_allocations.load();
    uint32_t sent = 0, echoed = 0;
    while (sent < kMessages) {
        for (uint32_t i = 0; i < 40 && sent < kMessages; ++i, ++sent) {
            Peer& client = s.client(sent % 4);
            const uint32_t size = 16 + (sent * 37) % (kMaxUnreliableMessageBytes - 16);
            std::memcpy(buffer.data(), &sent, 4);
            for (uint32_t b = 4; b < size; ++b) buffer[b] = uint8_t(sent + b);
            const Delivery delivery = sent % 5 == 0 ? Delivery::ReliableOrdered : Delivery::Unreliable;
            CHECK(
                client.transport->send(s.toHost[sent % 4], std::span(buffer.data(), size), delivery) ==
                SendResult::Queued);
        }
        const size_t seen = s.host.sink.messages.size();
        s.host.pump();
        for (size_t m = seen; m < s.host.sink.messages.size(); ++m)
            if (s.host.sink.messages[m].sequence % 10 == 0) {
                const Received& r = s.host.sink.messages[m];
                std::memcpy(buffer.data(), &r.sequence, 4);
                CHECK(
                    s.host.transport->send(r.connection, std::span(buffer.data(), 8), Delivery::ReliableOrdered) ==
                    SendResult::Queued);
                ++echoed;
            }
        for (auto& c : s.clients) c->pump();
        s.network.advance(8333);
    }
    while (s.host.pump().messages) {}
    for (auto& c : s.clients) c->pump();
    const uint64_t allocations = g_allocations.load() - before;
    if (allocations) std::fprintf(stderr, "  %llu allocations in the poll loop\n", (unsigned long long)allocations);
    CHECK(allocations == 0);

    CHECK(s.host.sink.messages.size() == kMessages);
    bool intact = true;
    for (const Received& r : s.host.sink.messages) intact &= r.intact;
    CHECK(intact);
    size_t echoes = 0;
    for (auto& c : s.clients) echoes += c->sink.messages.size();
    CHECK(echoed == kMessages / 10 && echoes == echoed);
    CHECK(s.host.mock->counts().receivedMessages == s.host.mock->counts().releasedMessages);
    CHECK(s.host.mock->counts().outstandingMessages == 0 && s.host.mock->counts().badReleases == 0);
    CHECK(s.host.transport->counters().messagesReleased == kMessages);
}

int main() {
    testConnect();
    testListenFailures();
    testRefusals();
    testMessages();
    testSendResults();
    testReleaseAndBudgets();
    testCloseAndStale();
    testOverflowReconciles();
    testBroadcast();
    testShutdownWithTransports();
    testNoAllocationPollLoop();
    if (failures) {
        std::fprintf(stderr, "test_sdr_transport_mock: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_sdr_transport_mock: all checks passed\n");
    return 0;
}
