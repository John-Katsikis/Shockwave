#include <Network/Loopback.h>

#include <Network/Messages.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace shockwave::net {

namespace {

enum class PacketKind : uint8_t { Data, ConnectRequest, Accept, Close, ConnectFailed };
enum class Pool : uint8_t { None, Small, Large };

struct Packet {
    PacketKind kind = PacketKind::Data;
    Delivery delivery = Delivery::ReliableOrdered;
    Pool pool = Pool::None;
    ReasonCode reason = ReasonCode::Normal;
    uint32_t buffer = 0, length = 0;
    uint32_t srcEndpoint = 0, srcSlot = 0, srcGeneration = 0;
    uint32_t dstEndpoint = 0, dstSlot = 0, dstGeneration = 0;   // dstGeneration 0: find by source
    uint32_t listenGeneration = 0;
    PeerId srcPeer;
};

struct Arrival {
    uint64_t atUs, seq;
    uint32_t packet;
    // std::push_heap makes a max-heap; this puts the earliest arrival on top.
    friend bool operator<(const Arrival& a, const Arrival& b) {
        return a.atUs != b.atUs ? a.atUs > b.atUs : a.seq > b.seq;
    }
};

constexpr uint32_t kNoEndpoint = UINT32_MAX;

struct Slot {
    ConnectionState state = ConnectionState::None;
    uint32_t generation = 0;
    uint32_t remoteEndpoint = kNoEndpoint, remoteSlot = 0, remoteGeneration = 0;
    PeerId remotePeer;
    FaultLink out;
    uint32_t pendingReliable = 0, pendingUnreliable = 0;
    uint64_t incomingPoll = 0;   // the poll that announced an incoming connection
    uint64_t messagesSent = 0, bytesSent = 0, messagesReceived = 0, bytesReceived = 0;
};

struct LocalEvent {
    TransportEventKind kind;
    uint32_t slot, generation;
};

class LoopbackTransport;

struct Endpoint {
    LoopbackTransport* owner = nullptr;   // null once destroyed
    PeerId identity;
    uint32_t endpointGeneration = 1;
    uint32_t nextGeneration = 1;
    bool listening = false;
    TransportAddress listenAddress;
    TransportLimits limits;
    uint32_t listenGeneration = 0;
    uint64_t pollSerial = 0;
    std::array<Slot, kMaxTransportConnections> slots;
    std::vector<Arrival> inbox;   // reserved up front, never grows past it
    std::array<LocalEvent, 2 * kMaxTransportConnections> events{};
    uint32_t eventCount = 0;
};

}   // namespace

struct LoopbackHub::Impl {
    LoopbackHubConfig config;
    uint64_t now = 0;
    uint64_t seq = 0;
    std::vector<Packet> packets;
    std::vector<uint32_t> freePackets, freeSmall, freeLarge;
    std::vector<uint8_t> smallBytes, largeBytes;
    std::vector<std::unique_ptr<Endpoint>> endpoints;   // never removed, so indices stay valid
    std::vector<std::array<uint64_t, 2>> outages;
    LoopbackStats stats;

    explicit Impl(const LoopbackHubConfig& c) : config(c) {
        const uint32_t total = c.smallPackets + c.largePackets + c.controlPackets;
        packets.resize(total);
        freePackets.reserve(total);
        for (uint32_t i = total; i-- > 0;) freePackets.push_back(i);
        smallBytes.resize(size_t(c.smallPackets) * kMaxUnreliableMessageBytes);
        largeBytes.resize(size_t(c.largePackets) * kMaxReliableMessageBytes);
        freeSmall.reserve(c.smallPackets);
        for (uint32_t i = c.smallPackets; i-- > 0;) freeSmall.push_back(i);
        freeLarge.reserve(c.largePackets);
        for (uint32_t i = c.largePackets; i-- > 0;) freeLarge.push_back(i);
    }

    uint32_t totalPackets() const { return uint32_t(packets.size()); }

    uint8_t* bufferData(const Packet& p) {
        if (p.pool == Pool::Small) return smallBytes.data() + size_t(p.buffer) * kMaxUnreliableMessageBytes;
        if (p.pool == Pool::Large) return largeBytes.data() + size_t(p.buffer) * kMaxReliableMessageBytes;
        return nullptr;
    }

    // A packet entry, with a buffer when it carries bytes. UINT32_MAX when the pool is out.
    uint32_t allocate(uint32_t bytes) {
        if (freePackets.empty()) return fail();
        Pool pool = Pool::None;
        uint32_t buffer = 0;
        if (bytes > 0) {
            if (bytes <= kMaxUnreliableMessageBytes && !freeSmall.empty()) {
                pool = Pool::Small;
                buffer = freeSmall.back();
                freeSmall.pop_back();
            } else if (!freeLarge.empty()) {
                pool = Pool::Large;
                buffer = freeLarge.back();
                freeLarge.pop_back();
            } else {
                return fail();
            }
        }
        const uint32_t index = freePackets.back();
        freePackets.pop_back();
        packets[index] = {};
        packets[index].pool = pool;
        packets[index].buffer = buffer;
        packets[index].length = bytes;
        stats.packetsInFlight = totalPackets() - uint32_t(freePackets.size());
        stats.packetsInFlightHigh = std::max(stats.packetsInFlightHigh, stats.packetsInFlight);
        stats.smallBuffersHigh = std::max(stats.smallBuffersHigh, config.smallPackets - uint32_t(freeSmall.size()));
        stats.largeBuffersHigh = std::max(stats.largeBuffersHigh, config.largePackets - uint32_t(freeLarge.size()));
        return index;
    }
    uint32_t fail() {
        ++stats.poolExhausted;
        return UINT32_MAX;
    }
    void release(uint32_t index) {
        Packet& p = packets[index];
        if (p.pool == Pool::Small) freeSmall.push_back(p.buffer);
        if (p.pool == Pool::Large) freeLarge.push_back(p.buffer);
        p.pool = Pool::None;
        freePackets.push_back(index);
        stats.packetsInFlight = totalPackets() - uint32_t(freePackets.size());
    }

    void deliverAt(uint32_t index, uint64_t atUs) {
        Packet& p = packets[index];
        Endpoint& target = *endpoints[p.dstEndpoint];
        if (!target.owner) {
            release(index);
            return;
        }
        target.inbox.push_back({atUs, seq++, index});
        std::push_heap(target.inbox.begin(), target.inbox.end());
    }

    // A control packet through the sender's link, in order with its reliable data.
    void control(
        PacketKind kind, Slot& link, uint32_t srcEndpoint, uint32_t srcSlot, uint32_t srcGeneration,
        uint32_t dstEndpoint, uint32_t dstSlot, uint32_t dstGeneration, ReasonCode reason) {
        const uint32_t index = allocate(0);
        if (index == UINT32_MAX) return;
        Packet& p = packets[index];
        p.kind = kind;
        p.reason = reason;
        p.srcEndpoint = srcEndpoint;
        p.srcSlot = srcSlot;
        p.srcGeneration = srcGeneration;
        p.srcPeer = endpoints[srcEndpoint]->identity;
        p.dstEndpoint = dstEndpoint;
        p.dstSlot = dstSlot;
        p.dstGeneration = dstGeneration;
        deliverAt(index, link.out.schedule(now, 0, Delivery::ReliableOrdered).atUs[0]);
    }

    uint64_t linkSeed(uint32_t endpoint, uint32_t slot, uint32_t generation) const {
        DeterministicRandom mix(config.seed ^ (uint64_t(endpoint) << 40) ^ (uint64_t(slot) << 32) ^ generation);
        return mix.next();
    }
    void newLink(Slot& slot, uint32_t endpoint, uint32_t index) {
        slot.out = FaultLink(config.faults, linkSeed(endpoint, index, slot.generation));
        for (const auto& [start, end] : outages) slot.out.addOutage(start, end - start);
    }
};

namespace {

class LoopbackTransport final : public ITransport {
public:
    LoopbackTransport(LoopbackHub::Impl* hub, uint32_t index) : m_hub(hub), m_index(index) {}
    ~LoopbackTransport() override {
        if (!m_hub) return;
        closeAll(ReasonCode::Normal);
        Endpoint& e = self();
        for (const Arrival& a : e.inbox) m_hub->release(a.packet);
        e.inbox.clear();
        e.owner = nullptr;
    }

    void detach() { m_hub = nullptr; }

    PeerId localPeer() const override { return m_hub ? m_hub->endpoints[m_index]->identity : PeerId{}; }
    uint32_t endpointGeneration() const override { return m_hub ? m_hub->endpoints[m_index]->endpointGeneration : 0; }

    NetResult<void> listen(const TransportAddress& address, const TransportLimits& limits) override {
        if (!m_hub) return NetError{NetErrorCode::Unavailable, "the loopback hub is gone"};
        if (address.kind != TransportAddress::Loopback)
            return NetError{NetErrorCode::InvalidArgument, "not a loopback address"};
        if (limits.maxConnections < 1 || limits.maxConnections > kMaxTransportConnections ||
            limits.maxMessageBytes < kHeaderBytes || limits.maxMessageBytes > kMaxReliableMessageBytes)
            return NetError{NetErrorCode::InvalidArgument, "transport limits out of range"};
        for (const auto& other : m_hub->endpoints)
            if (other.get() != &self() && other->owner && other->listening &&
                other->listenAddress.value == address.value && other->listenAddress.port == address.port)
                return NetError{NetErrorCode::AddressInUse, "another endpoint listens on that address"};
        Endpoint& e = self();
        e.listening = true;
        e.listenAddress = address;
        e.limits = limits;
        ++e.listenGeneration;
        ++e.endpointGeneration;
        return {};
    }

    NetResult<ConnectionId> connect(const TransportAddress& address) override {
        if (!m_hub) return NetError{NetErrorCode::Unavailable, "the loopback hub is gone"};
        if (address.kind != TransportAddress::Loopback)
            return NetError{NetErrorCode::InvalidArgument, "not a loopback address"};
        const uint32_t index = freeSlot();
        if (index == UINT32_MAX) return NetError{NetErrorCode::CapacityExceeded, "no free connection slot"};
        Endpoint& e = self();
        Slot& slot = e.slots[index];
        slot = {};
        slot.state = ConnectionState::Connecting;
        slot.generation = e.nextGeneration++;
        m_hub->newLink(slot, m_index, index);

        uint32_t listener = UINT32_MAX;
        for (uint32_t i = 0; i < m_hub->endpoints.size(); ++i) {
            const Endpoint& other = *m_hub->endpoints[i];
            if (other.owner && other.listening && other.listenAddress.value == address.value &&
                other.listenAddress.port == address.port)
                listener = i;
        }
        if (listener == UINT32_MAX) {
            // Nobody there: the connection fails a moment later, as a real one would.
            m_hub->control(
                PacketKind::ConnectFailed, slot, m_index, index, slot.generation, m_index, index, slot.generation,
                ReasonCode::ServiceUnavailable);
        } else {
            slot.remoteEndpoint = listener;
            slot.remotePeer = m_hub->endpoints[listener]->identity;
            const uint32_t packet = m_hub->allocate(0);
            if (packet != UINT32_MAX) {
                Packet& p = m_hub->packets[packet];
                p.kind = PacketKind::ConnectRequest;
                p.srcEndpoint = m_index;
                p.srcSlot = index;
                p.srcGeneration = slot.generation;
                p.srcPeer = e.identity;
                p.dstEndpoint = listener;
                p.listenGeneration = m_hub->endpoints[listener]->listenGeneration;
                m_hub->deliverAt(packet, slot.out.schedule(m_hub->now, 0, Delivery::ReliableOrdered).atUs[0]);
            }
        }
        return ConnectionId{index, slot.generation};
    }

    NetResult<void> accept(ConnectionId id) override {
        Slot* slot = find(id);
        if (!slot) return NetError{NetErrorCode::NotConnected, "no such connection"};
        if (slot->state != ConnectionState::Incoming)
            return NetError{NetErrorCode::InvalidState, "the connection isn't waiting to be accepted"};
        slot->state = ConnectionState::Connected;
        m_hub->control(
            PacketKind::Accept, *slot, m_index, id.slot, id.generation, slot->remoteEndpoint, slot->remoteSlot,
            slot->remoteGeneration, ReasonCode::Normal);
        pushEvent(TransportEventKind::Connected, id.slot, id.generation);
        return {};
    }

    void close(ConnectionId id, ReasonCode reason) override {
        Slot* slot = find(id);
        if (!slot) return;
        Endpoint& e = self();
        if (slot->remoteEndpoint != kNoEndpoint && m_hub->endpoints[slot->remoteEndpoint]->owner) {
            // A Connecting slot doesn't know the listener's slot yet: the listener finds
            // it by our side.
            const bool known = slot->state != ConnectionState::Connecting;
            m_hub->control(
                PacketKind::Close, *slot, m_index, id.slot, id.generation, slot->remoteEndpoint,
                known ? slot->remoteSlot : 0, known ? slot->remoteGeneration : 0, reason);
        }
        e.slots[id.slot] = {};
        // Drop local events for it that haven't been polled.
        uint32_t kept = 0;
        for (uint32_t i = 0; i < e.eventCount; ++i)
            if (!(e.events[i].slot == id.slot && e.events[i].generation == id.generation))
                e.events[kept++] = e.events[i];
        e.eventCount = kept;
    }

    void closeAll(ReasonCode reason) override {
        if (!m_hub) return;
        Endpoint& e = self();
        for (uint32_t i = 0; i < e.slots.size(); ++i)
            if (e.slots[i].state != ConnectionState::None) close({i, e.slots[i].generation}, reason);
        e.listening = false;
        ++e.endpointGeneration;
    }

    PollResult poll(TransportSink& sink, const ReceiveBudget& budget) override {
        PollResult result;
        if (!m_hub) return result;
        Endpoint& e = self();
        ++e.pollSerial;

        // Incoming connections nobody accepted or rejected by the next pump are refused.
        for (uint32_t i = 0; i < e.slots.size(); ++i)
            if (e.slots[i].state == ConnectionState::Incoming && e.slots[i].incomingPoll < e.pollSerial)
                close({i, e.slots[i].generation}, ReasonCode::Timeout);

        drainEvents(sink, result);
        while (!e.inbox.empty() && e.inbox.front().atUs <= m_hub->now) {
            const Packet& next = m_hub->packets[e.inbox.front().packet];
            if (next.kind == PacketKind::Data) {
                if (result.messages >= budget.maxMessages ||
                    (result.messages > 0 && result.bytes + next.length > budget.maxBytes)) {
                    result.budgetExhausted = true;
                    break;
                }
            }
            std::pop_heap(e.inbox.begin(), e.inbox.end());
            const uint32_t index = e.inbox.back().packet;
            e.inbox.pop_back();
            handle(index, sink, result);
            m_hub->release(index);
        }
        // Accepting during this poll queued a Connected event: deliver it now.
        drainEvents(sink, result);
        return result;
    }

    SendResult send(ConnectionId id, std::span<const uint8_t> bytes, Delivery delivery) override {
        Slot* slot = find(id);
        if (!slot || slot->state != ConnectionState::Connected) return SendResult::NotConnected;
        const Endpoint& e = self();
        const uint32_t limit =
            std::min(maxMessageBytes(delivery), e.listening ? e.limits.maxMessageBytes : kMaxReliableMessageBytes);
        if (bytes.empty() || bytes.size() > limit) return SendResult::Failed;
        const uint32_t size = uint32_t(bytes.size());
        const bool reliable = delivery == Delivery::ReliableOrdered;
        if (reliable && slot->pendingReliable + size > m_hub->config.maxPendingReliableBytes)
            return SendResult::Backpressure;
        if (!reliable && slot->pendingUnreliable + size > m_hub->config.maxPendingUnreliableBytes)
            return SendResult::Dropped;

        const FaultLink::Arrivals arrivals = slot->out.schedule(m_hub->now, size, delivery);
        ++slot->messagesSent;
        slot->bytesSent += size;
        for (uint8_t copy = 0; copy < arrivals.copies; ++copy) {
            const uint32_t index = m_hub->allocate(size);
            if (index == UINT32_MAX) return reliable ? SendResult::Backpressure : SendResult::Dropped;
            Packet& p = m_hub->packets[index];
            p.kind = PacketKind::Data;
            p.delivery = delivery;
            p.srcEndpoint = m_index;
            p.srcSlot = id.slot;
            p.srcGeneration = id.generation;
            p.srcPeer = e.identity;
            p.dstEndpoint = slot->remoteEndpoint;
            p.dstSlot = slot->remoteSlot;
            p.dstGeneration = slot->remoteGeneration;
            std::memcpy(m_hub->bufferData(p), bytes.data(), size);
            (reliable ? slot->pendingReliable : slot->pendingUnreliable) += size;
            m_hub->deliverAt(index, arrivals.atUs[copy]);
        }
        return SendResult::Queued;
    }

    std::optional<ConnectionStats> stats(ConnectionId id) const override {
        const Slot* slot = const_cast<LoopbackTransport*>(this)->find(id);
        if (!slot) return std::nullopt;
        ConnectionStats stats;
        stats.state = slot->state;
        stats.peer = slot->remotePeer;
        stats.rttUs = 2 * slot->out.profile().latencyUs;
        stats.pendingReliableBytes = slot->pendingReliable;
        stats.pendingUnreliableBytes = slot->pendingUnreliable;
        stats.messagesSent = slot->messagesSent;
        stats.bytesSent = slot->bytesSent;
        stats.messagesReceived = slot->messagesReceived;
        stats.bytesReceived = slot->bytesReceived;
        return stats;
    }

private:
    Endpoint& self() { return *m_hub->endpoints[m_index]; }
    const Endpoint& self() const { return *m_hub->endpoints[m_index]; }

    Slot* find(ConnectionId id) {
        if (!m_hub || !id.valid() || id.slot >= kMaxTransportConnections) return nullptr;
        Slot& slot = self().slots[id.slot];
        if (slot.state == ConnectionState::None || slot.generation != id.generation) return nullptr;
        return &slot;
    }

    uint32_t freeSlot() {
        const Endpoint& e = self();
        const uint32_t limit = e.listening ? e.limits.maxConnections : kMaxTransportConnections;
        uint32_t used = 0;
        for (const Slot& s : e.slots) used += s.state != ConnectionState::None;
        if (used >= limit) return UINT32_MAX;
        for (uint32_t i = 0; i < e.slots.size(); ++i)
            if (e.slots[i].state == ConnectionState::None) return i;
        return UINT32_MAX;
    }

    void drainEvents(TransportSink& sink, PollResult& result) {
        Endpoint& e = self();
        while (e.eventCount > 0) {
            const LocalEvent local = e.events[0];
            std::memmove(e.events.data(), e.events.data() + 1, (e.eventCount - 1) * sizeof(LocalEvent));
            --e.eventCount;
            TransportEvent event;
            event.kind = local.kind;
            event.connection = {local.slot, local.generation};
            event.endpointGeneration = e.endpointGeneration;
            event.peer = e.slots[local.slot].remotePeer;
            ++result.events;
            sink.onEvent(event);
        }
    }

    void pushEvent(TransportEventKind kind, uint32_t slot, uint32_t generation) {
        Endpoint& e = self();
        if (e.eventCount == e.events.size()) {
            SW_LOG(LogNetwork, Warning, "Loopback endpoint event queue is full; dropping an event");
            return;
        }
        e.events[e.eventCount++] = {kind, slot, generation};
    }

    // The sender's slot if it's still the connection that sent the packet.
    Slot* sender(const Packet& p) {
        Endpoint& src = *m_hub->endpoints[p.srcEndpoint];
        Slot& slot = src.slots[p.srcSlot];
        if (!src.owner || slot.state == ConnectionState::None || slot.generation != p.srcGeneration) return nullptr;
        return &slot;
    }

    void closed(uint32_t index, CloseCause cause, ReasonCode reason, TransportSink& sink, PollResult& result) {
        Endpoint& e = self();
        TransportEvent event;
        event.kind = TransportEventKind::Closed;
        event.connection = {index, e.slots[index].generation};
        event.endpointGeneration = e.endpointGeneration;
        event.peer = e.slots[index].remotePeer;
        event.cause = cause;
        event.reason = reason;
        e.slots[index] = {};
        ++result.events;
        sink.onEvent(event);
    }

    void handle(uint32_t packetIndex, TransportSink& sink, PollResult& result) {
        // A copy: the sink may send, and sending can't reuse this entry until it's released.
        const Packet p = m_hub->packets[packetIndex];
        Endpoint& e = self();
        switch (p.kind) {
        case PacketKind::Data: {
            Slot* from = sender(p);
            Slot& slot = e.slots[p.dstSlot];
            if (from)
                (p.delivery == Delivery::ReliableOrdered ? from->pendingReliable : from->pendingUnreliable) -= p.length;
            if (!from || slot.state != ConnectionState::Connected || slot.generation != p.dstGeneration) {
                ++m_hub->stats.staleDiscarded;
                return;
            }
            ++slot.messagesReceived;
            slot.bytesReceived += p.length;
            ++m_hub->stats.delivered;
            ReceivedMessage message;
            message.connection = {p.dstSlot, p.dstGeneration};
            message.endpointGeneration = e.endpointGeneration;
            message.peer = slot.remotePeer;
            message.delivery = p.delivery;
            message.receivedUs = m_hub->now;
            message.bytes = {m_hub->bufferData(p), p.length};
            ++result.messages;
            result.bytes += p.length;
            sink.onMessage(message);
            return;
        }
        case PacketKind::ConnectRequest: {
            Slot* from = sender(p);
            if (!from || from->state != ConnectionState::Connecting) return;
            if (!e.listening || e.listenGeneration != p.listenGeneration) {
                m_hub->control(
                    PacketKind::ConnectFailed, *from, p.srcEndpoint, p.srcSlot, p.srcGeneration, p.srcEndpoint,
                    p.srcSlot, p.srcGeneration, ReasonCode::ServiceUnavailable);
                return;
            }
            const uint32_t index = freeSlot();
            if (index == UINT32_MAX) {
                // Full: refused before the application ever sees it.
                m_hub->control(
                    PacketKind::Close, *from, m_index, 0, 0, p.srcEndpoint, p.srcSlot, p.srcGeneration,
                    ReasonCode::ServerFull);
                return;
            }
            Slot& slot = e.slots[index];
            slot = {};
            slot.state = ConnectionState::Incoming;
            slot.generation = e.nextGeneration++;
            slot.remoteEndpoint = p.srcEndpoint;
            slot.remoteSlot = p.srcSlot;
            slot.remoteGeneration = p.srcGeneration;
            slot.remotePeer = p.srcPeer;
            slot.incomingPoll = e.pollSerial;
            m_hub->newLink(slot, m_index, index);
            TransportEvent event;
            event.kind = TransportEventKind::Incoming;
            event.connection = {index, slot.generation};
            event.endpointGeneration = e.endpointGeneration;
            event.peer = p.srcPeer;
            ++result.events;
            sink.onEvent(event);
            return;
        }
        case PacketKind::Accept: {
            Slot& slot = e.slots[p.dstSlot];
            if (slot.state != ConnectionState::Connecting || slot.generation != p.dstGeneration || !sender(p)) return;
            slot.state = ConnectionState::Connected;
            slot.remoteSlot = p.srcSlot;
            slot.remoteGeneration = p.srcGeneration;
            TransportEvent event;
            event.kind = TransportEventKind::Connected;
            event.connection = {p.dstSlot, slot.generation};
            event.endpointGeneration = e.endpointGeneration;
            event.peer = slot.remotePeer;
            ++result.events;
            sink.onEvent(event);
            return;
        }
        case PacketKind::Close:
        case PacketKind::ConnectFailed: {
            uint32_t index = UINT32_MAX;
            if (p.dstGeneration != 0) {
                const Slot& slot = e.slots[p.dstSlot];
                if (slot.state != ConnectionState::None && slot.generation == p.dstGeneration) index = p.dstSlot;
            } else {
                for (uint32_t i = 0; i < e.slots.size(); ++i) {
                    const Slot& slot = e.slots[i];
                    if (slot.state != ConnectionState::None && slot.remoteEndpoint == p.srcEndpoint &&
                        slot.remoteSlot == p.srcSlot && slot.remoteGeneration == p.srcGeneration)
                        index = i;
                }
            }
            if (index == UINT32_MAX) return;
            CloseCause cause = CloseCause::ClosedByPeer;
            if (p.kind == PacketKind::ConnectFailed)
                cause = CloseCause::ProblemDetectedLocally;
            else if (e.slots[index].state == ConnectionState::Connecting)
                cause = CloseCause::Rejected;
            closed(index, cause, p.reason, sink, result);
            return;
        }
        }
    }

    LoopbackHub::Impl* m_hub;
    uint32_t m_index;
};

}   // namespace

LoopbackHub::LoopbackHub(const LoopbackHubConfig& config) : m_impl(std::make_unique<Impl>(config)) {}

LoopbackHub::~LoopbackHub() {
    for (auto& endpoint : m_impl->endpoints)
        if (endpoint->owner) endpoint->owner->detach();
}

std::unique_ptr<ITransport> LoopbackHub::createTransport(PeerId identity) {
    auto endpoint = std::make_unique<Endpoint>();
    endpoint->identity = identity;
    endpoint->inbox.reserve(m_impl->totalPackets());
    const uint32_t index = uint32_t(m_impl->endpoints.size());
    auto transport = std::make_unique<LoopbackTransport>(m_impl.get(), index);
    endpoint->owner = transport.get();
    m_impl->endpoints.push_back(std::move(endpoint));
    return transport;
}

uint64_t LoopbackHub::nowUs() const { return m_impl->now; }
void LoopbackHub::advance(uint64_t us) { m_impl->now += us; }

void LoopbackHub::setFaults(const FaultProfile& faults) {
    m_impl->config.faults = faults;
    for (auto& endpoint : m_impl->endpoints)
        for (Slot& slot : endpoint->slots)
            if (slot.state != ConnectionState::None) slot.out.setProfile(faults);
}

void LoopbackHub::addOutage(uint64_t startUs, uint64_t durationUs) {
    m_impl->outages.push_back({startUs, startUs + durationUs});
    for (auto& endpoint : m_impl->endpoints)
        for (Slot& slot : endpoint->slots)
            if (slot.state != ConnectionState::None) slot.out.addOutage(startUs, durationUs);
}

LoopbackStats LoopbackHub::stats() const { return m_impl->stats; }

}   // namespace shockwave::net
