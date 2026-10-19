#pragma once

// SteamNetworkAdapter's implementation, shared with SteamServicesImpl, which routes Steam's
// connection state changes to the adapter that owns each connection.

#include <SteamServices/SteamApi.h>
#include <SteamServices/SteamNetworkAdapter.h>

#include <array>
#include <cstdint>
#include <vector>

namespace shockwave::net {

class SteamNetworkAdapter::SteamNetworkAdapterImpl {
public:
    // Steam returns at most this many messages per receive call (plan section 4.4).
    static constexpr uint32_t kReceiveBatch = 32;
    // Each connection changes state a handful of times; more than this between two polls
    // means something is badly wrong, and poll() re-reads every connection's state.
    static constexpr uint32_t kChangeQueue = 4 * kMaxTransportConnections;

    SteamNetworkAdapterImpl(SteamServicesImpl& services, SteamApi& api, const SteamTransportSettings& settings);
    ~SteamNetworkAdapterImpl();

    // From SteamServicesImpl, inside SteamApi::runCallbacks().
    bool owns(const SteamConnectionChange& change) const;
    void queue(const SteamConnectionChange& change);
    // SteamServices is shutting down: close everything while Steam still runs, then forget it.
    void detach();

    struct Slot {
        ConnectionState state = ConnectionState::None;
        uint32_t generation = 0;
        uint32_t handle = 0;
        PeerId peer;
        bool outgoing = false;   // our connect(), not our listener's
        bool everConnected = false;
        uint64_t incomingPoll = 0;   // the poll that announced it
        uint32_t rttUs = 0;
        uint32_t pendingReliable = 0, pendingUnreliable = 0;
        uint64_t messagesSent = 0, bytesSent = 0, messagesReceived = 0, bytesReceived = 0;
    };

    // A received message copied out of Steam, waiting for the sink.
    struct Held {
        uint32_t slot = 0, generation = 0;
        uint32_t offset = 0, size = 0;
        Delivery delivery = Delivery::ReliableOrdered;
        uint64_t receivedUs = 0;
    };

    SteamServicesImpl* services = nullptr;   // null once detached
    SteamApi* api = nullptr;
    SteamTransportSettings settings;
    uint32_t endpointGeneration = 1;
    uint32_t nextGeneration = 1;
    uint32_t listenSocket = 0;
    uint32_t pollGroup = 0;
    bool listening = false;
    TransportLimits limits;
    uint64_t pollSerial = 0;
    std::array<Slot, kMaxTransportConnections> slots{};

    std::array<SteamConnectionChange, kChangeQueue> changes{};
    uint32_t changeHead = 0, changeCount = 0;
    bool changeOverflow = false;

    std::array<SteamMessage, kReceiveBatch> batch{};
    std::array<Held, kReceiveBatch> held{};
    uint32_t heldNext = 0, heldCount = 0;
    std::vector<uint8_t> arena;   // kReceiveBatch largest messages, sized once

    SteamAdapterCounters counters;

    Slot* find(ConnectionId id);
    const Slot* find(ConnectionId id) const;
    int32_t slotOf(uint32_t handle) const;
    uint32_t freeSlot() const;
    uint32_t activeCount() const;
    void freeSlotAt(uint32_t index);
    NetResult<void> ensurePollGroup();

    NetResult<void> listen(const TransportAddress& address, const TransportLimits& limits);
    NetResult<ConnectionId> connect(const TransportAddress& address);
    NetResult<void> accept(ConnectionId id);
    void close(ConnectionId id, ReasonCode reason);
    void closeAll(ReasonCode reason);
    PollResult poll(TransportSink& sink, const ReceiveBudget& budget);
    SendResult send(ConnectionId id, std::span<const uint8_t> bytes, Delivery delivery);
    std::optional<ConnectionStats> stats(ConnectionId id) const;

private:
    void handleChange(const SteamConnectionChange& change, TransportSink& sink, PollResult& result);
    void reconcile(TransportSink& sink, PollResult& result);
    void closed(uint32_t index, CloseCause cause, ReasonCode reason, TransportSink& sink, PollResult& result);
    void refreshStatus();
    uint32_t receiveBatch();
    void dropHeldFor(uint32_t slot);
};

}   // namespace shockwave::net
