#pragma once

// SteamNetworkAdapter: ITransport over Steam Datagram Relay (ISteamNetworkingSockets P2P),
// made by SteamServices::createSteamTransport(). Peers are SteamID64s; a listen address's
// port is the Steam virtual port.
//
// - listen() opens a P2P listen socket; connect() calls ConnectP2P. Every connection goes
//   into one poll group once it's connected.
// - Steam's connection state changes are queued as they arrive in framePass() and acted
//   on in poll(): Incoming for a peer connecting to our listener (accept() or close() it
//   before the next poll, or it's refused), Connected once, and Closed once.
// - poll() receives at most 32 messages at a time from the poll group, copies them into
//   a ring buffer allocated when the adapter is made, and releases every Steam message
//   at once, whatever happens to it. That's one bounded copy per message, not zero copy.
//   Messages left over when the budget runs out are delivered by the next poll.
// - send() sends reliable messages with k_nSteamNetworkingSend_ReliableNoNagle and
//   unreliable ones with k_nSteamNetworkingSend_UnreliableNoNagle. Unreliable messages
//   are at most SteamServicesSettings::maxUnreliableBytes (1152), and are dropped while
//   more than maxPendingSendBytes (32 KiB) is waiting to go on that connection.
// - Once the adapter exists, polling and sending allocate nothing.
//
// Close reasons travel as Steam end reasons kSteamAppEndReasonBase + ReasonCode.

#include <Network/Transport.h>

#include <cstdint>
#include <memory>
#include <span>

namespace shockwave::net {

class SteamApi;
class SteamServicesImpl;

struct SteamTransportSettings {
    uint16_t virtualPort = 0;   // when a listen or connect address has port 0
    uint32_t maxUnreliableBytes = 1152;
    uint32_t maxPendingSendBytes = 32768;
};

struct SteamAdapterCounters {
    uint64_t messagesReleased = 0;   // every Steam message received, released once each
    uint64_t droppedUnknownConnection = 0;   // for a connection that's closed or not connected
    uint64_t droppedOversize = 0;   // empty, or larger than its delivery allows
    uint64_t ignoredStateChanges = 0;   // stale handles, our own connect's Connecting
    uint64_t refusedIncoming = 0;   // full, no identity, or not accepted in time
    uint64_t stateChangeOverflows = 0;   // the queue was full; poll() re-read the states
    uint64_t sendFailures = 0;
    uint64_t unreliableBackpressure = 0;
    uint64_t pollGroupFailures = 0;
};

struct BroadcastResult {
    uint32_t queued = 0, dropped = 0, backpressure = 0, failed = 0;
};

class SteamNetworkAdapter final : public ITransport {
public:
    ~SteamNetworkAdapter() override;
    SteamNetworkAdapter(const SteamNetworkAdapter&) = delete;
    SteamNetworkAdapter& operator=(const SteamNetworkAdapter&) = delete;

    PeerId localPeer() const override;
    uint32_t endpointGeneration() const override;
    NetResult<void> listen(const TransportAddress& address, const TransportLimits& limits) override;
    NetResult<ConnectionId> connect(const TransportAddress& address) override;
    NetResult<void> accept(ConnectionId connection) override;
    void close(ConnectionId connection, ReasonCode reason) override;
    void closeAll(ReasonCode reason) override;
    PollResult poll(TransportSink& sink, const ReceiveBudget& budget) override;
    SendResult send(ConnectionId connection, std::span<const uint8_t> bytes, Delivery delivery) override;
    std::optional<ConnectionStats> stats(ConnectionId connection) const override;

    // send() to every connected peer but `except`.
    BroadcastResult broadcast(std::span<const uint8_t> bytes, Delivery delivery, ConnectionId except = {});
    SteamAdapterCounters counters() const;
    // False once its SteamServices has shut down: everything then fails or does nothing.
    bool attached() const;

    class SteamNetworkAdapterImpl;

private:
    friend class SteamServicesImpl;
    SteamNetworkAdapter(SteamServicesImpl& services, SteamApi& api, const SteamTransportSettings& settings);
    std::unique_ptr<SteamNetworkAdapterImpl> m_impl;
};

}   // namespace shockwave::net
