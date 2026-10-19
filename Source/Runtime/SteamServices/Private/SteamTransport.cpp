// SteamNetworkAdapter: the transport state table of docs/SteamMultiplayerImplementation.md
// section 4.4, over SteamApi. No Steam header: the SDK calls are in SteamSdk.cpp.
#include "SteamTransport.h"

#include "SteamServicesImpl.h"

#include <Network/Messages.h>

#include <algorithm>
#include <cstring>

namespace shockwave::net {

using Impl = SteamNetworkAdapter::SteamNetworkAdapterImpl;

namespace {

int32_t endReason(ReasonCode reason) { return kSteamAppEndReasonBase + int32_t(reason); }

ReasonCode reasonFrom(int32_t steamEndReason, ReasonCode otherwise) {
    const int32_t code = steamEndReason - kSteamAppEndReasonBase;
    return code >= 0 && code <= int32_t(kLastReasonCode) ? ReasonCode(code) : otherwise;
}

// Releases a batch of Steam messages when it goes out of scope, so every message is
// released exactly once, whatever happens to it.
class ReleaseBatch {
public:
    ReleaseBatch(SteamApi& api, std::span<SteamMessage> messages, uint64_t& released)
        : m_api(api), m_messages(messages), m_released(released) {}
    ~ReleaseBatch() {
        for (SteamMessage& m : m_messages)
            if (m.handle) {
                m_api.releaseMessage(m.handle);
                m.handle = nullptr;
                ++m_released;
            }
    }
    ReleaseBatch(const ReleaseBatch&) = delete;
    ReleaseBatch& operator=(const ReleaseBatch&) = delete;

private:
    SteamApi& m_api;
    std::span<SteamMessage> m_messages;
    uint64_t& m_released;
};

}   // namespace

Impl::SteamNetworkAdapterImpl(SteamServicesImpl& owner, SteamApi& steam, const SteamTransportSettings& s)
    : services(&owner), api(&steam), settings(s) {
    settings.maxUnreliableBytes = std::clamp<uint32_t>(settings.maxUnreliableBytes, 1, kMaxUnreliableMessageBytes);
    arena.resize(size_t(kReceiveBatch) * kMaxReliableMessageBytes);
}

Impl::~SteamNetworkAdapterImpl() {
    if (!services) return;
    closeAll(ReasonCode::Normal);
    if (pollGroup) api->destroyPollGroup(pollGroup);
    services->unregisterAdapter(this);
}

void Impl::detach() {
    if (!services) return;
    closeAll(ReasonCode::ServiceUnavailable);
    if (pollGroup) api->destroyPollGroup(pollGroup);
    pollGroup = 0;
    services = nullptr;
    api = nullptr;
}

Impl::Slot* Impl::find(ConnectionId id) {
    if (id.slot >= slots.size() || !id.valid()) return nullptr;
    Slot& s = slots[id.slot];
    return s.state != ConnectionState::None && s.generation == id.generation ? &s : nullptr;
}

const Impl::Slot* Impl::find(ConnectionId id) const { return const_cast<Impl*>(this)->find(id); }

int32_t Impl::slotOf(uint32_t handle) const {
    if (!handle) return -1;
    for (uint32_t i = 0; i < slots.size(); ++i)
        if (slots[i].state != ConnectionState::None && slots[i].handle == handle) return int32_t(i);
    return -1;
}

uint32_t Impl::freeSlot() const {
    for (uint32_t i = 0; i < slots.size(); ++i)
        if (slots[i].state == ConnectionState::None) return i;
    return UINT32_MAX;
}

uint32_t Impl::activeCount() const {
    uint32_t n = 0;
    for (const Slot& s : slots) n += s.state != ConnectionState::None;
    return n;
}

void Impl::freeSlotAt(uint32_t index) {
    dropHeldFor(index);
    slots[index] = {};
}

void Impl::dropHeldFor(uint32_t slot) {
    for (uint32_t i = heldNext; i < heldCount; ++i)
        if (held[i].slot == slot) held[i].generation = 0;   // skipped when its turn comes
}

bool Impl::owns(const SteamConnectionChange& change) const {
    if (slotOf(change.connection) >= 0) return true;
    return change.state == SteamConnectionState::Connecting && change.listenSocket != 0 &&
           change.listenSocket == listenSocket;
}

void Impl::queue(const SteamConnectionChange& change) {
    if (changeCount == changes.size()) {
        changeOverflow = true;
        ++counters.stateChangeOverflows;
        return;
    }
    changes[(changeHead + changeCount++) % changes.size()] = change;
}

NetResult<void> Impl::ensurePollGroup() {
    if (pollGroup) return {};
    pollGroup = api->createPollGroup();
    if (!pollGroup) {
        ++counters.pollGroupFailures;
        return NetError{NetErrorCode::Io, "Steam couldn't create a poll group"};
    }
    return {};
}

NetResult<void> Impl::listen(const TransportAddress& address, const TransportLimits& l) {
    if (!services) return NetError{NetErrorCode::Unavailable, "Steam has shut down"};
    if (address.kind != TransportAddress::SteamPeer)
        return NetError{NetErrorCode::InvalidArgument, "not a Steam address"};
    if (l.maxConnections < 1 || l.maxConnections > kMaxTransportConnections || l.maxMessageBytes < kHeaderBytes ||
        l.maxMessageBytes > kMaxReliableMessageBytes)
        return NetError{NetErrorCode::InvalidArgument, "transport limits out of range"};
    if (listening) return NetError{NetErrorCode::InvalidState, "already listening; closeAll() first"};
    const bool hadGroup = pollGroup != 0;
    if (NetResult<void> group = ensurePollGroup(); !group) return group;
    const uint16_t port = address.port ? address.port : settings.virtualPort;
    listenSocket = api->createListenSocketP2P(port);
    if (!listenSocket) {
        if (!hadGroup) {
            api->destroyPollGroup(pollGroup);
            pollGroup = 0;
        }
        SW_LOG(LogNetwork, Warning, "Steam couldn't listen on virtual port {}", port);
        return NetError{NetErrorCode::AddressInUse, "Steam couldn't open a P2P listen socket on that virtual port"};
    }
    listening = true;
    limits = l;
    ++endpointGeneration;
    return {};
}

NetResult<ConnectionId> Impl::connect(const TransportAddress& address) {
    if (!services) return NetError{NetErrorCode::Unavailable, "Steam has shut down"};
    if (address.kind != TransportAddress::SteamPeer || address.value == 0)
        return NetError{NetErrorCode::InvalidArgument, "not a SteamID"};
    if (address.value == api->localSteamId()) return NetError{NetErrorCode::InvalidArgument, "that's our own SteamID"};
    const uint32_t index = freeSlot();
    if (index == UINT32_MAX) return NetError{NetErrorCode::CapacityExceeded, "no free connection slot"};
    if (NetResult<void> group = ensurePollGroup(); !group) return group.error();
    const uint32_t handle = api->connectP2P(address.value, address.port ? address.port : settings.virtualPort);
    if (!handle) return NetError{NetErrorCode::Io, "Steam couldn't start connecting"};
    Slot& slot = slots[index];
    slot = {};
    slot.state = ConnectionState::Connecting;
    slot.generation = nextGeneration++;
    slot.handle = handle;
    slot.peer = PeerId{address.value};
    slot.outgoing = true;
    return ConnectionId{index, slot.generation};
}

NetResult<void> Impl::accept(ConnectionId id) {
    Slot* slot = find(id);
    if (!slot || !services) return NetError{NetErrorCode::NotConnected, "no such connection"};
    if (slot->state != ConnectionState::Incoming)
        return NetError{NetErrorCode::InvalidState, "the connection isn't waiting to be accepted"};
    if (api->acceptConnection(slot->handle) != SteamResult::Ok) {
        api->closeConnection(slot->handle, endReason(ReasonCode::ServiceUnavailable), false);
        freeSlotAt(id.slot);
        return NetError{NetErrorCode::Io, "Steam couldn't accept the connection"};
    }
    slot->state = ConnectionState::Connecting;   // Connected follows from Steam
    return {};
}

void Impl::close(ConnectionId id, ReasonCode reason) {
    Slot* slot = find(id);
    if (!slot || !services) return;
    // Linger so a final reliable message (Goodbye, Reject) still goes out.
    api->closeConnection(slot->handle, endReason(reason), slot->state == ConnectionState::Connected);
    freeSlotAt(id.slot);
}

void Impl::closeAll(ReasonCode reason) {
    if (!services) return;
    for (uint32_t i = 0; i < slots.size(); ++i)
        if (slots[i].state != ConnectionState::None) close({i, slots[i].generation}, reason);
    if (listenSocket) api->closeListenSocket(listenSocket);
    listenSocket = 0;
    listening = false;
    changeHead = changeCount = 0;
    changeOverflow = false;
    heldNext = heldCount = 0;
    ++endpointGeneration;
}

void Impl::closed(uint32_t index, CloseCause cause, ReasonCode reason, TransportSink& sink, PollResult& result) {
    Slot& slot = slots[index];
    // Steam keeps a closed connection's resources until we close it too.
    api->closeConnection(slot.handle, endReason(reason), false);
    TransportEvent event;
    event.kind = TransportEventKind::Closed;
    event.connection = {index, slot.generation};
    event.endpointGeneration = endpointGeneration;
    event.peer = slot.peer;
    event.cause = cause;
    event.reason = reason;
    freeSlotAt(index);
    ++result.events;
    sink.onEvent(event);
}

void Impl::handleChange(const SteamConnectionChange& change, TransportSink& sink, PollResult& result) {
    const int32_t known = slotOf(change.connection);
    switch (change.state) {
    case SteamConnectionState::Connecting: {
        if (known >= 0) {   // our own connect(), or one we already announced: keep waiting
            ++counters.ignoredStateChanges;
            return;
        }
        if (!listening || change.listenSocket != listenSocket) {   // a listener we closed
            ++counters.ignoredStateChanges;
            return;
        }
        const uint32_t index = freeSlot();
        if (change.remoteSteamId == 0 || index == UINT32_MAX || activeCount() >= limits.maxConnections) {
            api->closeConnection(
                change.connection,
                endReason(change.remoteSteamId ? ReasonCode::ServerFull : ReasonCode::AuthenticationFailed), false);
            ++counters.refusedIncoming;
            return;
        }
        Slot& slot = slots[index];
        slot = {};
        slot.state = ConnectionState::Incoming;
        slot.generation = nextGeneration++;
        slot.handle = change.connection;
        slot.peer = PeerId{change.remoteSteamId};
        slot.incomingPoll = pollSerial;
        TransportEvent event;
        event.kind = TransportEventKind::Incoming;
        event.connection = {index, slot.generation};
        event.endpointGeneration = endpointGeneration;
        event.peer = slot.peer;
        ++result.events;
        sink.onEvent(event);
        return;
    }
    case SteamConnectionState::Connected: {
        if (known < 0) {
            ++counters.ignoredStateChanges;
            return;
        }
        Slot& slot = slots[uint32_t(known)];
        if (slot.state != ConnectionState::Connecting) {   // already Connected, or never accepted
            ++counters.ignoredStateChanges;
            return;
        }
        if (!api->setConnectionPollGroup(slot.handle, pollGroup)) {
            ++counters.pollGroupFailures;
            closed(uint32_t(known), CloseCause::ProblemDetectedLocally, ReasonCode::ServiceUnavailable, sink, result);
            return;
        }
        slot.state = ConnectionState::Connected;
        slot.everConnected = true;
        TransportEvent event;
        event.kind = TransportEventKind::Connected;
        event.connection = {uint32_t(known), slot.generation};
        event.endpointGeneration = endpointGeneration;
        event.peer = slot.peer;
        ++result.events;
        sink.onEvent(event);
        return;
    }
    case SteamConnectionState::ClosedByPeer:
    case SteamConnectionState::ProblemDetectedLocally: {
        if (known < 0) {
            ++counters.ignoredStateChanges;
            return;
        }
        const Slot& slot = slots[uint32_t(known)];
        CloseCause cause = CloseCause::ClosedByPeer;
        ReasonCode reason = reasonFrom(change.endReason, ReasonCode::Normal);
        if (change.state == SteamConnectionState::ProblemDetectedLocally) {
            cause = CloseCause::ProblemDetectedLocally;
            reason = reasonFrom(change.endReason, ReasonCode::Timeout);
        } else if (slot.outgoing && !slot.everConnected) {
            cause = CloseCause::Rejected;
        }
        closed(uint32_t(known), cause, reason, sink, result);
        return;
    }
    case SteamConnectionState::FindingRoute:   // progress; the connect deadline still runs
    case SteamConnectionState::None:   // what follows our own close
        return;
    }
}

void Impl::reconcile(TransportSink& sink, PollResult& result) {
    // Some changes were lost: read every connection's state from Steam instead.
    changeOverflow = false;
    for (uint32_t i = 0; i < slots.size(); ++i) {
        const Slot& slot = slots[i];
        if (slot.state == ConnectionState::None) continue;
        SteamConnectionStatus status;
        SteamConnectionChange change;
        change.connection = slot.handle;
        change.remoteSteamId = slot.peer.value;
        change.state =
            api->connectionStatus(slot.handle, status) ? status.state : SteamConnectionState::ProblemDetectedLocally;
        if (change.state != SteamConnectionState::Connecting) handleChange(change, sink, result);
    }
}

void Impl::refreshStatus() {
    for (Slot& slot : slots) {
        if (slot.state != ConnectionState::Connected) continue;
        SteamConnectionStatus status;
        if (!api->connectionStatus(slot.handle, status)) continue;
        slot.rttUs = status.pingMs >= 0 ? uint32_t(status.pingMs) * 1000u : 0u;
        slot.pendingReliable = status.pendingReliableBytes;
        slot.pendingUnreliable = status.pendingUnreliableBytes;
    }
}

uint32_t Impl::receiveBatch() {
    heldNext = heldCount = 0;
    const int received = api->receiveMessagesOnPollGroup(pollGroup, batch);
    if (received <= 0) return 0;
    const auto messages = std::span(batch).first(std::min<size_t>(size_t(received), batch.size()));
    ReleaseBatch release(*api, messages, counters.messagesReleased);
    uint32_t offset = 0;
    for (const SteamMessage& m : messages) {
        const int32_t index = slotOf(m.connection);
        if (index < 0 || slots[uint32_t(index)].state != ConnectionState::Connected) {
            ++counters.droppedUnknownConnection;
            continue;
        }
        const Delivery delivery = m.reliable ? Delivery::ReliableOrdered : Delivery::Unreliable;
        const uint32_t limit =
            m.reliable ? (listening ? limits.maxMessageBytes : kMaxReliableMessageBytes) : settings.maxUnreliableBytes;
        if (m.size == 0 || m.size > limit || !m.data) {
            ++counters.droppedOversize;
            continue;
        }
        std::memcpy(arena.data() + offset, m.data, m.size);
        Held& h = held[heldCount++];
        h.slot = uint32_t(index);
        h.generation = slots[uint32_t(index)].generation;
        h.offset = offset;
        h.size = m.size;
        h.delivery = delivery;
        h.receivedUs = m.receivedUs > 0 ? uint64_t(m.receivedUs) : 0;
        offset += m.size;
    }
    return uint32_t(messages.size());
}

PollResult Impl::poll(TransportSink& sink, const ReceiveBudget& budget) {
    PollResult result;
    if (!services) return result;
    ++pollSerial;

    // Incoming connections nobody accepted or rejected by the next poll are refused.
    for (uint32_t i = 0; i < slots.size(); ++i)
        if (slots[i].state == ConnectionState::Incoming && slots[i].incomingPoll < pollSerial) {
            api->closeConnection(slots[i].handle, endReason(ReasonCode::Timeout), false);
            ++counters.refusedIncoming;
            freeSlotAt(i);
        }

    // The sink may accept, close or closeAll while we go, so re-read the queue each time.
    while (changeCount > 0 && services) {
        const SteamConnectionChange change = changes[changeHead];
        changeHead = (changeHead + 1) % changes.size();
        --changeCount;
        handleChange(change, sink, result);
    }
    if (changeOverflow && services) reconcile(sink, result);
    if (!services || !pollGroup) return result;
    refreshStatus();

    for (;;) {
        if (heldNext == heldCount && receiveBatch() == 0) break;
        while (heldNext < heldCount) {
            const Held& h = held[heldNext];
            Slot& slot = slots[h.slot];
            if (h.generation == 0 || slot.generation != h.generation || slot.state != ConnectionState::Connected) {
                ++heldNext;   // closed since it arrived
                ++counters.droppedUnknownConnection;
                continue;
            }
            if (result.messages >= budget.maxMessages ||
                (result.messages > 0 && result.bytes + h.size > budget.maxBytes)) {
                result.budgetExhausted = true;   // the rest wait in the ring for the next poll
                return result;
            }
            ++heldNext;
            ReceivedMessage message;
            message.connection = {h.slot, h.generation};
            message.endpointGeneration = endpointGeneration;
            message.peer = slot.peer;
            message.delivery = h.delivery;
            message.receivedUs = h.receivedUs;
            message.bytes = std::span<const uint8_t>(arena.data() + h.offset, h.size);
            ++slot.messagesReceived;
            slot.bytesReceived += h.size;
            ++result.messages;
            result.bytes += h.size;
            sink.onMessage(message);
            if (!services) return result;   // the sink shut Steam down
        }
    }
    return result;
}

SendResult Impl::send(ConnectionId id, std::span<const uint8_t> bytes, Delivery delivery) {
    Slot* slot = find(id);
    if (!slot || !services || slot->state != ConnectionState::Connected) return SendResult::NotConnected;
    const bool reliable = delivery == Delivery::ReliableOrdered;
    const uint32_t limit =
        reliable ? (listening ? limits.maxMessageBytes : kMaxReliableMessageBytes) : settings.maxUnreliableBytes;
    if (bytes.empty() || bytes.size() > limit) return SendResult::Failed;
    const uint32_t size = uint32_t(bytes.size());
    if (!reliable && slot->pendingReliable + slot->pendingUnreliable + size > settings.maxPendingSendBytes) {
        ++counters.unreliableBackpressure;   // latest-only state: the next snapshot replaces it
        return SendResult::Dropped;
    }
    switch (
        api->sendMessage(slot->handle, bytes, reliable ? SteamSend::ReliableNoNagle : SteamSend::UnreliableNoNagle)) {
    case SteamResult::Ok:
        ++slot->messagesSent;
        slot->bytesSent += size;
        (reliable ? slot->pendingReliable : slot->pendingUnreliable) += size;   // until the next poll re-reads it
        return SendResult::Queued;
    case SteamResult::LimitExceeded:
        ++counters.sendFailures;
        return reliable ? SendResult::Backpressure : SendResult::Dropped;
    case SteamResult::NoConnection:
    case SteamResult::InvalidState:
        ++counters.sendFailures;
        return SendResult::NotConnected;   // the Closed event follows
    case SteamResult::InvalidParam:
    case SteamResult::Fail: break;
    }
    ++counters.sendFailures;
    return SendResult::Failed;
}

std::optional<ConnectionStats> Impl::stats(ConnectionId id) const {
    const Slot* slot = find(id);
    if (!slot) return std::nullopt;
    ConnectionStats s;
    s.state = slot->state;
    s.peer = slot->peer;
    s.rttUs = slot->rttUs;
    s.pendingReliableBytes = slot->pendingReliable;
    s.pendingUnreliableBytes = slot->pendingUnreliable;
    s.messagesSent = slot->messagesSent;
    s.bytesSent = slot->bytesSent;
    s.messagesReceived = slot->messagesReceived;
    s.bytesReceived = slot->bytesReceived;
    return s;
}

// The public class forwards to the implementation.

SteamNetworkAdapter::SteamNetworkAdapter(
    SteamServicesImpl& services, SteamApi& api, const SteamTransportSettings& settings)
    : m_impl(std::make_unique<SteamNetworkAdapterImpl>(services, api, settings)) {}
SteamNetworkAdapter::~SteamNetworkAdapter() = default;

PeerId SteamNetworkAdapter::localPeer() const { return m_impl->api ? PeerId{m_impl->api->localSteamId()} : PeerId{}; }
uint32_t SteamNetworkAdapter::endpointGeneration() const { return m_impl->endpointGeneration; }
NetResult<void> SteamNetworkAdapter::listen(const TransportAddress& address, const TransportLimits& limits) {
    return m_impl->listen(address, limits);
}
NetResult<ConnectionId> SteamNetworkAdapter::connect(const TransportAddress& address) {
    return m_impl->connect(address);
}
NetResult<void> SteamNetworkAdapter::accept(ConnectionId connection) { return m_impl->accept(connection); }
void SteamNetworkAdapter::close(ConnectionId connection, ReasonCode reason) { m_impl->close(connection, reason); }
void SteamNetworkAdapter::closeAll(ReasonCode reason) { m_impl->closeAll(reason); }
PollResult SteamNetworkAdapter::poll(TransportSink& sink, const ReceiveBudget& budget) {
    return m_impl->poll(sink, budget);
}
SendResult SteamNetworkAdapter::send(ConnectionId connection, std::span<const uint8_t> bytes, Delivery delivery) {
    return m_impl->send(connection, bytes, delivery);
}
std::optional<ConnectionStats> SteamNetworkAdapter::stats(ConnectionId connection) const {
    return m_impl->stats(connection);
}

BroadcastResult SteamNetworkAdapter::broadcast(std::span<const uint8_t> bytes, Delivery delivery, ConnectionId except) {
    BroadcastResult result;
    for (uint32_t i = 0; i < m_impl->slots.size(); ++i) {
        const SteamNetworkAdapterImpl::Slot& slot = m_impl->slots[i];
        const ConnectionId id{i, slot.generation};
        if (slot.state != ConnectionState::Connected || id == except) continue;
        switch (m_impl->send(id, bytes, delivery)) {
        case SendResult::Queued: ++result.queued; break;
        case SendResult::Dropped: ++result.dropped; break;
        case SendResult::Backpressure: ++result.backpressure; break;
        case SendResult::NotConnected:
        case SendResult::Failed: ++result.failed; break;
        }
    }
    return result;
}

SteamAdapterCounters SteamNetworkAdapter::counters() const { return m_impl->counters; }
bool SteamNetworkAdapter::attached() const { return m_impl->services != nullptr; }

}   // namespace shockwave::net
