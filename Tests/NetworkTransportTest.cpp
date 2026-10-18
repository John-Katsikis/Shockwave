// The loopback transport (Network/Loopback.h): connecting, accepting and rejecting;
// reliable order under delay and retries; unreliable loss, duplication and reordering;
// determinism by seed; send buffer lifetime; bounded queues and pools; stale events
// after close; receive budgets; outages and bandwidth limits; no allocation once
// connected. Everything runs on the hub's virtual clock.
#include <Network/Loopback.h>
#include <Network/Messages.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
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

static const TransportAddress kHostAddress{TransportAddress::Loopback, 1, 0};
static constexpr PeerId kHostPeer{1001}, kClientPeer{2001};

struct Received {
    ConnectionId connection;
    PeerId peer;
    Delivery delivery;
    uint32_t sequence;
    uint32_t size;
    uint64_t atUs;
};

// Records everything into storage reserved up front, so polling allocates nothing.
struct Recorder : TransportSink {
    std::vector<TransportEvent> events;
    std::vector<Received> messages;
    ITransport* acceptFrom = nullptr;   // accept every Incoming on this transport
    ReasonCode rejectWith = ReasonCode::Normal;
    bool reject = false;

    Recorder() {
        events.reserve(1024);
        messages.reserve(65536);
    }
    void onEvent(const TransportEvent& event) override {
        if (events.size() < events.capacity()) events.push_back(event);
        if (event.kind == TransportEventKind::Incoming && acceptFrom) {
            if (reject)
                acceptFrom->close(event.connection, rejectWith);
            else
                CHECK(bool(acceptFrom->accept(event.connection)));
        }
    }
    void onMessage(const ReceivedMessage& m) override {
        uint32_t sequence = 0;
        if (m.bytes.size() >= 4) sequence = loadU32(m.bytes.data());
        if (messages.size() < messages.capacity())
            messages.push_back({m.connection, m.peer, m.delivery, sequence, uint32_t(m.bytes.size()), m.receivedUs});
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

struct Pair {
    LoopbackHub& hub;
    std::unique_ptr<ITransport> host, client;
    Recorder hostSink, clientSink;
    ConnectionId toHost, toClient;

    explicit Pair(LoopbackHub& h, TransportLimits limits = {}) : hub(h) {
        host = hub.createTransport(kHostPeer);
        client = hub.createTransport(kClientPeer);
        CHECK(bool(host->listen(kHostAddress, limits)));
        hostSink.acceptFrom = host.get();
        auto id = client->connect(kHostAddress);
        CHECK(bool(id));
        if (id) toHost = id.value();
        pumpUntil([&] { return clientSink.count(TransportEventKind::Connected) > 0; });
        if (const TransportEvent* e = hostSink.last(TransportEventKind::Connected)) toClient = e->connection;
    }
    void pump(const ReceiveBudget& budget = {}) {
        host->poll(hostSink, budget);
        client->poll(clientSink, budget);
    }
    template <class Done>
    bool pumpUntil(Done done, uint64_t stepUs = 1000, int maxSteps = 100000) {
        for (int i = 0; i < maxSteps; ++i) {
            pump();
            if (done()) return true;
            hub.advance(stepUs);
        }
        return false;
    }
};

static std::vector<uint8_t> payload(uint32_t sequence, size_t size) {
    std::vector<uint8_t> bytes(std::max<size_t>(size, 4), uint8_t(sequence));
    storeU32(bytes.data(), sequence);
    return bytes;
}

static void testConnect() {
    LoopbackHub hub;
    Pair pair(hub);
    CHECK(pair.toHost.valid() && pair.toClient.valid());
    CHECK(pair.hostSink.count(TransportEventKind::Incoming) == 1);
    CHECK(pair.hostSink.count(TransportEventKind::Connected) == 1);
    CHECK(pair.clientSink.count(TransportEventKind::Connected) == 1);
    const TransportEvent* incoming = pair.hostSink.last(TransportEventKind::Incoming);
    CHECK(incoming && incoming->peer == kClientPeer);   // the identity comes from the transport
    CHECK(pair.clientSink.last(TransportEventKind::Connected)->peer == kHostPeer);
    CHECK(pair.hostSink.last(TransportEventKind::Connected)->endpointGeneration == pair.host->endpointGeneration());
    CHECK(pair.host->localPeer() == kHostPeer);

    const std::vector<uint8_t> hello = payload(7, 40);
    CHECK(pair.client->send(pair.toHost, hello, Delivery::ReliableOrdered) == SendResult::Queued);
    pair.pump();
    CHECK(pair.hostSink.messages.size() == 1);
    CHECK(pair.hostSink.messages[0].sequence == 7 && pair.hostSink.messages[0].size == 40);
    CHECK(pair.hostSink.messages[0].peer == kClientPeer);
    CHECK(pair.hostSink.messages[0].connection == pair.toClient);
    auto stats = pair.client->stats(pair.toHost);
    CHECK(stats && stats->state == ConnectionState::Connected && stats->messagesSent == 1 && stats->bytesSent == 40);
    CHECK(pair.host->stats(pair.toClient)->messagesReceived == 1);

    // A second listener can't take the same address.
    auto other = hub.createTransport(PeerId{3001});
    CHECK(other->listen(kHostAddress, {}).error().code == NetErrorCode::AddressInUse);
    CHECK(other->listen({TransportAddress::SteamPeer, 1, 0}, {}).error().code == NetErrorCode::InvalidArgument);
    CHECK(other->listen({TransportAddress::Loopback, 2, 0}, {17, 16384}).error().code == NetErrorCode::InvalidArgument);
}

static void testReliableOrder() {
    for (const FaultPreset& preset : faultPresets()) {
        LoopbackHubConfig config;
        config.seed = 99;
        config.faults = preset.profile;
        config.maxPendingReliableBytes = 1 << 20;
        config.smallPackets = 16384;
        LoopbackHub hub(config);
        Pair pair(hub);
        constexpr uint32_t count = 2000;
        uint32_t sent = 0;
        // Ten messages a millisecond, in both directions, while time moves on.
        while (sent < count) {
            for (int i = 0; i < 10 && sent < count; ++i, ++sent) {
                const std::vector<uint8_t> bytes = payload(sent, 16 + sent % 200);
                CHECK(pair.client->send(pair.toHost, bytes, Delivery::ReliableOrdered) == SendResult::Queued);
                CHECK(pair.host->send(pair.toClient, bytes, Delivery::ReliableOrdered) == SendResult::Queued);
            }
            pair.pump();
            hub.advance(1000);
        }
        pair.pumpUntil(
            [&] { return pair.hostSink.messages.size() >= count && pair.clientSink.messages.size() >= count; });
        for (const Recorder* sink : {&pair.hostSink, &pair.clientSink}) {
            CHECK(sink->messages.size() == count);
            bool ordered = true;
            for (uint32_t i = 0; i < sink->messages.size(); ++i)
                ordered &= sink->messages[i].sequence == i && sink->messages[i].delivery == Delivery::ReliableOrdered;
            if (!ordered) std::fprintf(stderr, "  %s: reliable messages out of order\n", preset.name);
            CHECK(ordered);
        }
        // Nothing left over.
        hub.advance(10'000'000);
        pair.pump();
        CHECK(pair.hostSink.messages.size() == count);
        CHECK(pair.client->stats(pair.toHost)->pendingReliableBytes == 0);
    }
}

struct UnreliableResult {
    size_t received = 0, unique = 0, duplicates = 0, reordered = 0;
    friend bool operator==(const UnreliableResult&, const UnreliableResult&) = default;
};

static UnreliableResult runUnreliable(const char* presetName, uint64_t seed) {
    LoopbackHubConfig config;
    config.seed = seed;
    config.faults = findFaultPreset(presetName)->profile;
    LoopbackHub hub(config);
    Pair pair(hub);
    constexpr uint32_t count = 5000;
    for (uint32_t i = 0; i < count; ++i) {
        const std::vector<uint8_t> bytes = payload(i, 100);
        CHECK(pair.client->send(pair.toHost, bytes, Delivery::Unreliable) == SendResult::Queued);
        pair.pump();
        hub.advance(1000);
    }
    hub.advance(2'000'000);
    pair.pump();
    UnreliableResult result;
    std::vector<int> seen(count, 0);
    uint32_t highest = 0;
    bool any = false;
    for (const Received& m : pair.hostSink.messages) {
        ++result.received;
        if (m.sequence >= count) continue;
        if (seen[m.sequence]++)
            ++result.duplicates;
        else
            ++result.unique;
        if (any && m.sequence < highest) ++result.reordered;
        highest = std::max(highest, m.sequence);
        any = true;
    }
    return result;
}

static void testUnreliableFaults() {
    const UnreliableResult clean = runUnreliable("clean", 5);
    CHECK(clean.received == 5000 && clean.unique == 5000 && clean.duplicates == 0 && clean.reordered == 0);

    const UnreliableResult hostile = runUnreliable("hostile", 5);
    std::fprintf(
        stderr, "  hostile: %zu received, %zu unique, %zu duplicates, %zu reordered of 5000\n", hostile.received,
        hostile.unique, hostile.duplicates, hostile.reordered);
    // 10% loss, 2% duplication and +-50 ms of jitter at 1 ms spacing.
    CHECK(hostile.unique > 4300 && hostile.unique < 4700);
    CHECK(hostile.duplicates > 40 && hostile.duplicates < 160);
    CHECK(hostile.reordered > 500);

    const UnreliableResult cafe = runUnreliable("cafe", 5);
    CHECK(cafe.unique > 4900 && cafe.unique < 4990);

    // Same seed, same deliveries; another seed, different ones.
    CHECK(runUnreliable("hostile", 5) == hostile);
    CHECK(!(runUnreliable("hostile", 6) == hostile));
}

static void testFaultLinkTiming() {
    FaultProfile profile;
    profile.latencyUs = 30000;
    profile.jitterUs = 10000;
    FaultLink link(profile, 42);
    uint64_t lastReliable = 0;
    for (int i = 0; i < 1000; ++i) {
        const uint64_t now = uint64_t(i) * 500;
        const FaultLink::Arrivals unreliable = link.schedule(now, 100, Delivery::Unreliable);
        CHECK(unreliable.copies == 1);
        CHECK(unreliable.atUs[0] >= now + 20000 && unreliable.atUs[0] <= now + 40000);
        const FaultLink::Arrivals reliable = link.schedule(now, 100, Delivery::ReliableOrdered);
        CHECK(reliable.copies == 1 && reliable.atUs[0] >= lastReliable);
        lastReliable = reliable.atUs[0];
    }

    // Jitter larger than the latency never makes a message arrive before it was sent.
    profile.latencyUs = 1000;
    profile.jitterUs = 50000;
    FaultLink wild(profile, 1);
    for (int i = 0; i < 1000; ++i) CHECK(wild.schedule(5000, 10, Delivery::Unreliable).atUs[0] >= 5000);

    // Reliable "loss" only delays.
    profile = {};
    profile.latencyUs = 10000;
    profile.reliableLoss = 0.5;
    profile.retryUs = 40000;
    FaultLink lossy(profile, 3);
    uint64_t delayed = 0;
    for (int i = 0; i < 1000; ++i) {
        const FaultLink::Arrivals a = lossy.schedule(0, 10, Delivery::ReliableOrdered);
        CHECK(a.copies == 1);
        if (a.atUs[0] > 10000) ++delayed;
    }
    CHECK(delayed > 300);
    CHECK(lossy.counters().retries > 300);

    CHECK(findFaultPreset("cafe") && findFaultPreset("cafe")->profile.latencyUs == 30000);
    CHECK(findFaultPreset("hostile")->durationTicks == 3600 && findFaultPreset("poor")->durationTicks == 7200);
    CHECK(!findFaultPreset("lan"));
}

static void testOutageAndBandwidth() {
    {
        LoopbackHub hub;
        Pair pair(hub);
        hub.addOutage(hub.nowUs(), 500'000);
        const std::vector<uint8_t> bytes = payload(1, 64);
        for (int i = 0; i < 10; ++i)
            CHECK(pair.client->send(pair.toHost, bytes, Delivery::Unreliable) == SendResult::Queued);
        CHECK(pair.client->send(pair.toHost, payload(2, 64), Delivery::ReliableOrdered) == SendResult::Queued);
        const uint64_t start = hub.nowUs();
        pair.pumpUntil([&] { return !pair.hostSink.messages.empty(); });
        CHECK(pair.hostSink.messages.size() == 1);   // the unreliable ones were lost
        CHECK(pair.hostSink.messages[0].sequence == 2);
        CHECK(pair.hostSink.messages[0].atUs >= start + 500'000);
    }
    {
        LoopbackHubConfig config;
        config.faults.bytesPerSecond = 16384;
        config.faults.maxQueueDelayUs = 250'000;
        config.maxPendingReliableBytes = 1 << 20;
        config.largePackets = 512;
        LoopbackHub hub(config);
        Pair pair(hub);
        const uint64_t start = hub.nowUs();
        for (uint32_t i = 0; i < 64; ++i)
            CHECK(pair.client->send(pair.toHost, payload(i, 1024), Delivery::ReliableOrdered) == SendResult::Queued);
        pair.pumpUntil([&] { return pair.hostSink.messages.size() == 64; });
        // 64 KiB at 16 KiB/s takes four seconds.
        CHECK(pair.hostSink.messages.back().atUs - start >= 3'900'000);
        CHECK(pair.hostSink.messages.back().atUs - start <= 4'200'000);

        // Unreliable messages that would queue behind more than 250 ms are dropped.
        pair.hostSink.clear();
        for (uint32_t i = 0; i < 20; ++i)
            CHECK(pair.client->send(pair.toHost, payload(i, 1024), Delivery::Unreliable) == SendResult::Queued);
        hub.advance(2'000'000);
        pair.pump();
        CHECK(pair.hostSink.messages.size() >= 3 && pair.hostSink.messages.size() <= 5);
    }
}

static void testSendBufferLifetime() {
    LoopbackHubConfig config;
    config.faults.latencyUs = 20000;
    LoopbackHub hub(config);
    Pair pair(hub);
    std::vector<uint8_t> buffer = payload(11, 32);
    CHECK(pair.client->send(pair.toHost, buffer, Delivery::Unreliable) == SendResult::Queued);
    // The caller may reuse the buffer the moment send() returns.
    storeU32(buffer.data(), 12);
    CHECK(pair.client->send(pair.toHost, buffer, Delivery::Unreliable) == SendResult::Queued);
    std::fill(buffer.begin(), buffer.end(), 0xEE);
    pair.pumpUntil([&] { return pair.hostSink.messages.size() == 2; });
    CHECK(pair.hostSink.messages.size() == 2);
    CHECK(pair.hostSink.messages[0].sequence == 11 && pair.hostSink.messages[1].sequence == 12);
}

static void testSizesAndQueues() {
    LoopbackHubConfig config;
    config.faults.latencyUs = 50000;
    config.maxPendingReliableBytes = 4096;
    config.maxPendingUnreliableBytes = 2048;
    LoopbackHub hub(config);
    Pair pair(hub);

    CHECK(pair.client->send(pair.toHost, {}, Delivery::ReliableOrdered) == SendResult::Failed);
    CHECK(
        pair.client->send(pair.toHost, payload(0, kMaxUnreliableMessageBytes + 1), Delivery::Unreliable) ==
        SendResult::Failed);
    CHECK(
        pair.client->send(pair.toHost, payload(0, kMaxUnreliableMessageBytes), Delivery::Unreliable) ==
        SendResult::Queued);
    CHECK(
        pair.client->send(pair.toHost, payload(0, kMaxReliableMessageBytes + 1), Delivery::ReliableOrdered) ==
        SendResult::Failed);

    // Pending reliable bytes are capped per connection: past it, backpressure.
    LoopbackHub hub2(config);
    Pair queue(hub2);
    int queued = 0;
    SendResult last = SendResult::Queued;
    for (int i = 0; i < 10 && last == SendResult::Queued; ++i) {
        last = queue.client->send(queue.toHost, payload(uint32_t(i), 1000), Delivery::ReliableOrdered);
        queued += last == SendResult::Queued;
    }
    CHECK(queued == 4 && last == SendResult::Backpressure);
    CHECK(queue.client->stats(queue.toHost)->pendingReliableBytes == 4000);
    CHECK(queue.client->send(queue.toHost, payload(9, 1000), Delivery::Unreliable) == SendResult::Queued);
    CHECK(queue.client->send(queue.toHost, payload(9, 1000), Delivery::Unreliable) == SendResult::Queued);
    CHECK(queue.client->send(queue.toHost, payload(9, 1000), Delivery::Unreliable) == SendResult::Dropped);
    // Once delivered, there's room again.
    queue.pumpUntil([&] { return queue.hostSink.messages.size() == 6; });
    CHECK(queue.client->stats(queue.toHost)->pendingReliableBytes == 0);
    CHECK(queue.client->send(queue.toHost, payload(5, 1000), Delivery::ReliableOrdered) == SendResult::Queued);

    // The hub's fixed pools run out cleanly too.
    LoopbackHubConfig tinyPools;
    tinyPools.faults.latencyUs = 50000;
    tinyPools.smallPackets = 8;
    tinyPools.largePackets = 0;
    LoopbackHub hub3(tinyPools);
    Pair small(hub3);
    int accepted = 0;
    for (int i = 0; i < 20; ++i)
        accepted += small.client->send(small.toHost, payload(1, 64), Delivery::ReliableOrdered) == SendResult::Queued;
    CHECK(accepted == 8);
    CHECK(small.client->send(small.toHost, payload(1, 64), Delivery::Unreliable) == SendResult::Dropped);
    CHECK(hub3.stats().poolExhausted > 0);
    CHECK(hub3.stats().smallBuffersHigh == 8);
}

static void testBudget() {
    LoopbackHub hub;
    Pair pair(hub);
    for (uint32_t i = 0; i < 10; ++i)
        CHECK(pair.client->send(pair.toHost, payload(i, 100), Delivery::ReliableOrdered) == SendResult::Queued);
    PollResult result = pair.host->poll(pair.hostSink, {3, 1 << 20});
    CHECK(result.messages == 3 && result.budgetExhausted);
    result = pair.host->poll(pair.hostSink, {100, 250});
    CHECK(result.messages == 2 && result.bytes == 200 && result.budgetExhausted);
    // One message bigger than the byte budget still gets through on its own.
    result = pair.host->poll(pair.hostSink, {100, 50});
    CHECK(result.messages == 1);
    result = pair.host->poll(pair.hostSink, {});
    CHECK(result.messages == 4 && !result.budgetExhausted);
    CHECK(pair.hostSink.messages.size() == 10);
    for (uint32_t i = 0; i < 10; ++i) CHECK(pair.hostSink.messages[i].sequence == i);
}

static void testCloseAndStaleGenerations() {
    LoopbackHubConfig config;
    config.faults.latencyUs = 30000;
    LoopbackHub hub(config);
    Pair pair(hub);
    pair.hostSink.clear();

    // Messages still in flight when their sender closes are dropped with the connection.
    for (uint32_t i = 0; i < 5; ++i)
        CHECK(pair.client->send(pair.toHost, payload(i, 50), Delivery::ReliableOrdered) == SendResult::Queued);
    const ConnectionId old = pair.toHost;
    pair.client->close(old, ReasonCode::Normal);
    // Invalid at once.
    CHECK(pair.client->send(old, payload(9, 50), Delivery::ReliableOrdered) == SendResult::NotConnected);
    CHECK(!pair.client->stats(old));
    pair.client->close(old, ReasonCode::Normal);   // closing twice is harmless
    hub.advance(1'000'000);
    pair.pump();
    CHECK(pair.hostSink.messages.empty());
    CHECK(pair.hostSink.count(TransportEventKind::Closed) == 1);
    const TransportEvent* closed = pair.hostSink.last(TransportEventKind::Closed);
    CHECK(closed && closed->cause == CloseCause::ClosedByPeer && closed->reason == ReasonCode::Normal);
    CHECK(closed->connection == pair.toClient);
    CHECK(pair.clientSink.count(TransportEventKind::Closed) == 0);   // no event for your own close
    CHECK(hub.stats().staleDiscarded == 5);
    // The host's old id is dead too.
    CHECK(pair.host->send(pair.toClient, payload(1, 10), Delivery::ReliableOrdered) == SendResult::NotConnected);

    // Reconnecting reuses the slot with a new generation.
    pair.clientSink.clear();
    pair.hostSink.clear();
    auto again = pair.client->connect(kHostAddress);
    CHECK(bool(again));
    CHECK(again->slot == old.slot && again->generation != old.generation);
    pair.pumpUntil([&] { return pair.clientSink.count(TransportEventKind::Connected) == 1; });
    const ConnectionId hostSide = pair.hostSink.last(TransportEventKind::Connected)->connection;
    CHECK(hostSide.slot == pair.toClient.slot && hostSide.generation != pair.toClient.generation);
    CHECK(pair.client->send(old, payload(1, 10), Delivery::ReliableOrdered) == SendResult::NotConnected);
    CHECK(pair.client->send(again.value(), payload(77, 10), Delivery::ReliableOrdered) == SendResult::Queued);
    pair.pumpUntil([&] { return !pair.hostSink.messages.empty(); });
    CHECK(pair.hostSink.messages.size() == 1 && pair.hostSink.messages[0].sequence == 77);
    CHECK(pair.hostSink.messages[0].connection == hostSide);

    // The host closing: in-flight messages both ways are dropped, the client hears once.
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK(pair.client->send(again.value(), payload(i, 10), Delivery::Unreliable) == SendResult::Queued);
        CHECK(pair.host->send(hostSide, payload(i, 10), Delivery::ReliableOrdered) == SendResult::Queued);
    }
    pair.hostSink.clear();
    pair.host->close(hostSide, ReasonCode::HostLeft);
    hub.advance(1'000'000);
    pair.pump();
    pair.pump();
    CHECK(pair.hostSink.messages.empty());
    CHECK(pair.clientSink.messages.empty());
    CHECK(pair.clientSink.count(TransportEventKind::Closed) == 1);
    CHECK(pair.clientSink.last(TransportEventKind::Closed)->reason == ReasonCode::HostLeft);

    // closeAll changes the endpoint generation and stops listening.
    const uint32_t generation = pair.host->endpointGeneration();
    pair.host->closeAll(ReasonCode::Normal);
    CHECK(pair.host->endpointGeneration() != generation);
    pair.clientSink.clear();
    auto refused = pair.client->connect(kHostAddress);
    CHECK(bool(refused));
    pair.pumpUntil([&] { return pair.clientSink.count(TransportEventKind::Closed) == 1; });
    CHECK(pair.clientSink.last(TransportEventKind::Closed)->cause == CloseCause::ProblemDetectedLocally);
    CHECK(bool(pair.host->listen(kHostAddress, {})));
}

static void testAcceptReject() {
    LoopbackHub hub;
    auto host = hub.createTransport(kHostPeer);
    CHECK(bool(host->listen(kHostAddress, {2, 16384})));
    Recorder hostSink;
    hostSink.acceptFrom = host.get();
    hostSink.reject = true;
    hostSink.rejectWith = ReasonCode::ContentMismatch;

    // Rejected by the application.
    auto client = hub.createTransport(kClientPeer);
    Recorder clientSink;
    auto id = client->connect(kHostAddress);
    CHECK(bool(id));
    for (int i = 0; i < 5; ++i) {
        host->poll(hostSink, {});
        client->poll(clientSink, {});
    }
    CHECK(hostSink.count(TransportEventKind::Incoming) == 1);
    CHECK(hostSink.count(TransportEventKind::Connected) == 0);
    const TransportEvent* closed = clientSink.last(TransportEventKind::Closed);
    CHECK(closed && closed->cause == CloseCause::Rejected && closed->reason == ReasonCode::ContentMismatch);
    CHECK(clientSink.count(TransportEventKind::Connected) == 0);
    CHECK(client->send(id.value(), payload(1, 10), Delivery::ReliableOrdered) == SendResult::NotConnected);

    // Not answered by the next pump: refused automatically.
    hostSink.acceptFrom = nullptr;
    clientSink.clear();
    hostSink.clear();
    id = client->connect(kHostAddress);
    host->poll(hostSink, {});
    CHECK(hostSink.count(TransportEventKind::Incoming) == 1);
    const ConnectionId pending = hostSink.last(TransportEventKind::Incoming)->connection;
    CHECK(host->stats(pending)->state == ConnectionState::Incoming);
    CHECK(host->send(pending, payload(1, 10), Delivery::ReliableOrdered) == SendResult::NotConnected);
    host->poll(hostSink, {});
    CHECK(!host->stats(pending));
    CHECK(host->accept(pending).error().code == NetErrorCode::NotConnected);
    client->poll(clientSink, {});
    CHECK(
        clientSink.last(TransportEventKind::Closed) &&
        clientSink.last(TransportEventKind::Closed)->reason == ReasonCode::Timeout);

    // Accepting twice is an error, not a second connection.
    hostSink.clear();
    clientSink.clear();
    id = client->connect(kHostAddress);
    host->poll(hostSink, {});
    const ConnectionId first = hostSink.last(TransportEventKind::Incoming)->connection;
    CHECK(bool(host->accept(first)));
    CHECK(host->accept(first).error().code == NetErrorCode::InvalidState);

    // At capacity, a new peer is refused before the application sees it.
    auto second = hub.createTransport(PeerId{2002});
    Recorder secondSink;
    CHECK(bool(second->connect(kHostAddress)));
    host->poll(hostSink, {});
    const ConnectionId secondId = hostSink.last(TransportEventKind::Incoming)->connection;
    CHECK(bool(host->accept(secondId)));
    auto third = hub.createTransport(PeerId{2003});
    Recorder thirdSink;
    CHECK(bool(third->connect(kHostAddress)));
    hostSink.clear();
    host->poll(hostSink, {});
    third->poll(thirdSink, {});
    CHECK(hostSink.count(TransportEventKind::Incoming) == 0);
    CHECK(
        thirdSink.last(TransportEventKind::Closed) &&
        thirdSink.last(TransportEventKind::Closed)->reason == ReasonCode::ServerFull &&
        thirdSink.last(TransportEventKind::Closed)->cause == CloseCause::Rejected);

    // Nobody listening.
    auto lonely = hub.createTransport(PeerId{2004});
    Recorder lonelySink;
    CHECK(bool(lonely->connect({TransportAddress::Loopback, 99, 0})));
    lonely->poll(lonelySink, {});
    CHECK(
        lonelySink.last(TransportEventKind::Closed) &&
        lonelySink.last(TransportEventKind::Closed)->cause == CloseCause::ProblemDetectedLocally);
}

static void testManyClients() {
    LoopbackHubConfig config;
    config.faults = findFaultPreset("cafe")->profile;
    LoopbackHub hub(config);
    auto host = hub.createTransport(kHostPeer);
    CHECK(bool(host->listen(kHostAddress, {11, 16384})));
    Recorder hostSink;
    hostSink.acceptFrom = host.get();
    std::vector<std::unique_ptr<ITransport>> clients;
    std::vector<Recorder> sinks(11);
    std::vector<ConnectionId> ids;
    for (uint64_t i = 0; i < 11; ++i) {
        clients.push_back(hub.createTransport(PeerId{5000 + i}));
        ids.push_back(clients.back()->connect(kHostAddress).value());
    }
    auto pumpAll = [&] {
        host->poll(hostSink, {});
        for (size_t i = 0; i < clients.size(); ++i) clients[i]->poll(sinks[i], {});
    };
    for (int step = 0; step < 1000; ++step) {
        pumpAll();
        hub.advance(1000);
    }
    CHECK(hostSink.count(TransportEventKind::Connected) == 11);
    for (const Recorder& sink : sinks) CHECK(sink.count(TransportEventKind::Connected) == 1);
    // Each client's messages arrive on its own connection, under its own identity.
    for (size_t i = 0; i < clients.size(); ++i)
        for (uint32_t m = 0; m < 20; ++m)
            CHECK(
                clients[i]->send(ids[i], payload(uint32_t(i * 100 + m), 30), Delivery::ReliableOrdered) ==
                SendResult::Queued);
    for (int step = 0; step < 1000; ++step) {
        pumpAll();
        hub.advance(1000);
    }
    CHECK(hostSink.messages.size() == 220);
    std::vector<uint32_t> next(11, 0);
    for (const Received& m : hostSink.messages) {
        const uint32_t client = m.sequence / 100;
        CHECK(client < 11 && m.peer == PeerId{5000 + client});
        CHECK(m.sequence % 100 == next[client]++);
    }
}

static void testHubGoneFirst() {
    std::unique_ptr<ITransport> host, client;
    {
        LoopbackHub hub;
        Pair pair(hub);
        host = std::move(pair.host);
        client = std::move(pair.client);
    }
    Recorder sink;
    CHECK(client->send({0, 1}, payload(1, 10), Delivery::ReliableOrdered) == SendResult::NotConnected);
    CHECK(client->poll(sink, {}).events == 0);
    CHECK(client->listen(kHostAddress, {}).error().code == NetErrorCode::Unavailable);
    CHECK(!client->connect(kHostAddress));
    client->closeAll(ReasonCode::Normal);
}

static void testNoAllocationWhenConnected() {
    LoopbackHubConfig config;
    config.faults = findFaultPreset("poor")->profile;
    LoopbackHub hub(config);
    Pair pair(hub);
    const std::vector<uint8_t> small = payload(1, 69 * 12 + 32), big = payload(2, 4000);
    const uint64_t before = g_allocations.load();
    for (int tick = 0; tick < 7200; ++tick) {
        pair.client->send(pair.toHost, small, Delivery::Unreliable);
        pair.host->send(pair.toClient, small, Delivery::Unreliable);
        if (tick % 30 == 0) pair.host->send(pair.toClient, big, Delivery::ReliableOrdered);
        pair.pump();
        hub.advance(16667);
    }
    const uint64_t allocations = g_allocations.load() - before;
    if (allocations) std::fprintf(stderr, "  %llu allocations after connecting\n", (unsigned long long)allocations);
    CHECK(allocations == 0);
    CHECK(pair.clientSink.messages.size() > 6500);
    CHECK(hub.stats().packetsInFlightHigh < 200);
}

int main() {
    testConnect();
    testReliableOrder();
    testUnreliableFaults();
    testFaultLinkTiming();
    testOutageAndBandwidth();
    testSendBufferLifetime();
    testSizesAndQueues();
    testBudget();
    testCloseAndStaleGenerations();
    testAcceptReject();
    testManyClients();
    testHubGoneFirst();
    testNoAllocationWhenConnected();
    if (failures) {
        std::fprintf(stderr, "network_transport: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("network_transport: all checks passed\n");
    return 0;
}
