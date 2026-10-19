#pragma once

// NetworkWorld: one Play session's network side, as game.network(). PlaySession always
// creates one: offline it reports local authority and does nothing at all (no transport,
// no service calls). Online it owns the session's transport and runs the connection
// handshake (docs/Networking.md, "Session integration"):
//
//   client                                   host
//   connect ----------------------------->   accept (at most 4 beyond capacity)
//   Hello (protocol, digests, scene) ---->   check them, and that there's room
//          <----------------------------    Challenge (the host's settings), or Reject
//   AuthTicket <------------------------->   AuthTicket   (mutual, through IOnlineServices)
//          <----------------------------    Welcome (player slot), once the host has
//                                            validated the client
//   Synchronizing, once the client has validated the host too
//
// The baseline that takes a client from Synchronizing to Active is replication's
// (plan Batch 5); until then a client waits in Synchronizing and its synchronization
// timeout still applies. The host is Active as soon as it listens.
//
// Role (Offline, Authority, Client) is chosen before GameMode::beginPlay and never
// changes until Stop. State moves on as the session does; a failure leaves it Failed
// with a ReasonCode until Stop.
//
// The world never finds its services or transport through a global: PlaySession hands it
// a NetworkStart and a non-owning IOnlineServices, which must outlive it. Everything runs
// on the main thread. close() shuts down in a fixed order (see close()) and is idempotent;
// after it, nothing more is reported, and late service results and transport events are
// ignored.

#include <Engine/Subsystem.h>
#include <Multiplayer/NetworkSettings.h>
#include <Network/Messages.h>
#include <Network/OnlineServices.h>
#include <Network/Transport.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace shockwave::net {

enum class NetworkRole : uint8_t { Offline, Authority, Client };
enum class NetworkState : uint8_t {
    Offline,
    Starting,
    Connecting,
    Authenticating,
    Synchronizing,
    Active,
    Closing,
    Failed
};

const char* networkRoleName(NetworkRole role);
const char* networkStateName(NetworkState state);

// What both sides compare before a client may join.
struct NetworkCompatibility {
    std::string gameProtocol = "shockwave";
    Digest schemaDigest{};   // ReplicationRegistry::schemaDigest
    Digest contentDigest{};   // computeContentDigest
    uint16_t sceneId = 1;   // the manifest's ID of the scene being played
};

// How a Play session goes online. Set on PlaySession before start().
struct NetworkStart {
    NetworkRole role = NetworkRole::Offline;
    NetworkSettings settings;   // tick_rate, max_players, timeouts and budgets
    NetworkCompatibility compatibility;
    TransportAddress address;   // the host listens on it; a client connects to it
    // Host: the lobby it made (closed to joins and left at Stop). Client: the lobby it
    // joined, sent in Hello. None for a developer direct join.
    LobbyId lobby;
    // Mutual session tickets through IOnlineServices. Only loopback tests and developer
    // direct joins may turn this off.
    bool authenticate = true;
    double synchronizeTimeoutSeconds = 30;   // Welcome to baseline
};

enum class NetworkEventKind : uint8_t {
    StateChanged,   // state() changed; `state` is the new one
    PlayerJoined,   // host: a client was admitted (Welcome sent) in `slot`
    PlayerLeft,   // host: an admitted client left or was dropped
};

struct NetworkEvent {
    NetworkEventKind kind = NetworkEventKind::StateChanged;
    NetworkState state = NetworkState::Offline;
    PeerId peer;
    uint16_t slot = 0;
    ReasonCode reason = ReasonCode::Normal;
};

struct NetworkWorldStats {
    uint64_t messagesReceived = 0, bytesReceived = 0, messagesSent = 0;
    uint64_t malformed = 0;   // decoding failed: the peer was rejected
    uint64_t rejected = 0;   // Reject sent (host) or received (client)
    uint64_t ignored = 0;   // valid, but for a later batch (replication, gameplay)
    uint64_t eventsDropped = 0;   // the event queue was full
    uint64_t staleEvents = 0;   // transport events for an old endpoint or connection
    uint32_t admitted = 0;   // host: clients currently welcomed
    uint32_t pending = 0;   // host: connections still in the handshake
};

class NetworkWorld final : public Subsystem {
public:
    // Offline: local authority, nothing to do.
    NetworkWorld();
    // Online; begin() starts it. services must outlive the world.
    NetworkWorld(const NetworkStart& start, IOnlineServices* services);
    ~NetworkWorld() override;
    NetworkWorld(const NetworkWorld&) = delete;
    NetworkWorld& operator=(const NetworkWorld&) = delete;

    // Makes the transport and listens (host) or connects (client). A failure releases
    // everything, leaves the state Failed and is returned; PlaySession then fails start().
    NetResult<void> begin();
    // Main thread, once per frame (in menus too): receives and handles messages up to the
    // settings' per-pump budget, and checks timeouts. nowSeconds is monotonic.
    void pump(double nowSeconds);
    // The start of a fixed tick: handles what arrived since the last pump, at the same
    // time, so a slow frame doesn't hold back a batch of ticks.
    void drain(uint32_t tick);
    // An event from the services, drained by the application. True when it belonged to
    // this session (and was handled); false for anything else, including everything
    // after close().
    bool handleOnlineEvent(const OnlineEvent& event);

    // Ends network play now with this reason (e.g. SimulationOverrun): says Goodbye,
    // releases the transport and auth sessions, and leaves the state Failed.
    void fail(ReasonCode reason, const char* why);
    // Shuts down in this order, and only once:
    //  1. state Closing, a new generation (late events are ignored from here on);
    //  2. the host's lobby closed to joins;
    //  3. Goodbye to every connected peer, best effort;
    //  4. every connection and the listener closed;
    //  5. whatever arrived since, drained and dropped;
    //  6. auth sessions ended and ticket requests cancelled;
    //  7. the lobby left;
    //  8. the transport destroyed, the peer table and event queue cleared.
    // The state ends Offline (or stays Failed). PlaySession calls it first in stop().
    void close(ReasonCode reason = ReasonCode::Normal);

    NetworkRole role() const { return m_role; }
    NetworkState state() const { return m_state; }
    bool online() const { return m_role != NetworkRole::Offline; }
    // The host, or offline play: this machine decides what happens.
    bool authority() const { return m_role != NetworkRole::Client; }
    ReasonCode failure() const { return m_failure; }
    const std::string& failureText() const { return m_failureText; }
    uint32_t generation() const { return m_generation; }
    // 1 for the host and offline play; a client's slot once welcomed, else 0.
    uint16_t localSlot() const { return m_localSlot; }
    bool hasTransport() const { return m_transport != nullptr; }
    const NetworkStart& start() const { return m_start; }
    const NetworkWorldStats& stats() const;

    // Local menus, overlays or an unfocused window: the game sends neutral input but the
    // session keeps running (online "pause").
    void setLocalInputSuppressed(bool suppressed) { m_inputSuppressed = suppressed; }
    bool localInputSuppressed() const { return m_inputSuppressed; }

    // The next event, oldest first. Up to kMaxEvents wait; past that the oldest go.
    bool pollEvent(NetworkEvent& out);
    static constexpr uint32_t kMaxEvents = 64;

    // Host: handshake limits (plan 6.5).
    static constexpr uint32_t kMaxPendingBeyondCapacity = 4;
    static constexpr double kPreAuthBytesPerSecond = 16384;
    static constexpr double kPreAuthBurstBytes = 8192;

    struct Impl;

private:
    void setState(NetworkState state);
    void push(const NetworkEvent& event);
    void release(ReasonCode reason);

    NetworkStart m_start;
    IOnlineServices* m_services = nullptr;
    std::unique_ptr<ITransport> m_transport;
    std::unique_ptr<Impl> m_impl;
    NetworkRole m_role = NetworkRole::Offline;
    NetworkState m_state = NetworkState::Offline;
    ReasonCode m_failure = ReasonCode::Normal;
    std::string m_failureText;
    uint32_t m_generation = 1;
    uint16_t m_localSlot = 1;
    bool m_inputSuppressed = false;
    bool m_closed = false;
    std::array<NetworkEvent, kMaxEvents> m_events{};
    uint32_t m_eventHead = 0, m_eventCount = 0;
};

}   // namespace shockwave::net
