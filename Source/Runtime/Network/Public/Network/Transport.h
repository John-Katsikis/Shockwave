#pragma once

// ITransport: one networking session endpoint, the only thing gameplay networking sees
// of Steam or the loopback test hub.
//
// - Main thread only; every call returns without blocking.
// - One send() is one message on the other side: there's no stream framing.
// - send() borrows its bytes only until it returns; the caller may reuse the buffer.
// - poll() hands received messages to a sink. A message's bytes stay valid until the
//   sink returns; copy what you keep.
// - close() invalidates the ConnectionId at once, even if the backend lingers. Events
//   carry the connection's generation and the endpoint generation, so an event from an
//   old connection can't be mistaken for a new one that reuses its slot.

#include <Network/NetworkTypes.h>

#include <cstdint>
#include <optional>
#include <span>

namespace shockwave::net {

enum class Delivery : uint8_t { ReliableOrdered, Unreliable };
enum class SendResult : uint8_t { Queued, Dropped, Backpressure, NotConnected, Failed };

const char* deliveryName(Delivery delivery);
const char* sendResultName(SendResult result);

// A connection on one endpoint: a slot, and the generation of the connection using it.
// Generation 0 is never used, so a default ConnectionId is invalid.
struct ConnectionId {
    uint32_t slot = 0;
    uint32_t generation = 0;
    bool valid() const { return generation != 0; }
    friend bool operator==(ConnectionId, ConnectionId) = default;
};

struct TransportAddress {
    enum Kind : uint8_t { SteamPeer, Loopback };
    Kind kind = Loopback;
    uint64_t value = 0;   // SteamID64, or a loopback hub address
    uint16_t port = 0;   // Steam virtual port
};

// The most connections any endpoint holds: eleven remote players plus up to four peers
// still being admitted.
inline constexpr uint32_t kMaxTransportConnections = 16;

struct TransportLimits {
    uint32_t maxConnections = 11;
    uint32_t maxMessageBytes = 16384;
};

struct ReceiveBudget {
    uint32_t maxMessages = 256;
    uint32_t maxBytes = 262144;
};

enum class TransportEventKind : uint8_t {
    Incoming,   // a peer wants to connect to our listener: accept() or close() it this pump
    Connected,   // the transport is usable; this is not application-level readiness
    Closed,   // terminal, sent once; the ConnectionId is no longer valid
};

enum class CloseCause : uint8_t {
    ClosedByPeer,
    ProblemDetectedLocally,   // no route, no listener, or the transport went away
    Rejected,   // our connect() was refused
};

struct TransportEvent {
    TransportEventKind kind = TransportEventKind::Connected;
    ConnectionId connection;
    uint32_t endpointGeneration = 0;
    PeerId peer;
    CloseCause cause = CloseCause::ClosedByPeer;   // Closed only
    ReasonCode reason = ReasonCode::Normal;   // Closed only: what the peer gave
};

struct ReceivedMessage {
    ConnectionId connection;
    uint32_t endpointGeneration = 0;
    PeerId peer;
    Delivery delivery = Delivery::ReliableOrdered;
    uint64_t receivedUs = 0;
    std::span<const uint8_t> bytes;   // valid until the sink returns
};

class TransportSink {
public:
    virtual ~TransportSink() = default;
    virtual void onEvent(const TransportEvent& event) = 0;
    virtual void onMessage(const ReceivedMessage& message) = 0;
};

struct PollResult {
    uint32_t events = 0;
    uint32_t messages = 0;
    uint32_t bytes = 0;
    bool budgetExhausted = false;   // more was ready; poll again next pump
};

enum class ConnectionState : uint8_t { None, Connecting, Incoming, Connected };

struct ConnectionStats {
    ConnectionState state = ConnectionState::None;
    PeerId peer;
    uint32_t rttUs = 0;   // the transport's estimate; 0 if it has none
    uint32_t pendingReliableBytes = 0;   // sent, not yet delivered
    uint32_t pendingUnreliableBytes = 0;
    uint64_t messagesSent = 0, bytesSent = 0;
    uint64_t messagesReceived = 0, bytesReceived = 0;
};

class ITransport {
public:
    virtual ~ITransport() = default;

    virtual PeerId localPeer() const = 0;
    // Changes on listen() and closeAll(), so events from before can be told apart.
    virtual uint32_t endpointGeneration() const = 0;

    virtual NetResult<void> listen(const TransportAddress& address, const TransportLimits& limits) = 0;
    // Returns at once; a Connected or Closed event follows.
    virtual NetResult<ConnectionId> connect(const TransportAddress& address) = 0;
    virtual NetResult<void> accept(ConnectionId connection) = 0;
    virtual void close(ConnectionId connection, ReasonCode reason) = 0;
    // Closes every connection and stops listening.
    virtual void closeAll(ReasonCode reason) = 0;

    virtual PollResult poll(TransportSink& sink, const ReceiveBudget& budget) = 0;
    virtual SendResult send(ConnectionId connection, std::span<const uint8_t> bytes, Delivery delivery) = 0;
    virtual std::optional<ConnectionStats> stats(ConnectionId connection) const = 0;
};

}   // namespace shockwave::net
