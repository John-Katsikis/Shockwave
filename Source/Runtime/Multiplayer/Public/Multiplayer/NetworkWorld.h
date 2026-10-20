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
//          <----------------------------    the baseline: BaselineBegin, a Spawn per
//                                            network object, BaselineEnd (with a digest)
//   Ready (the baseline's digest) ------>
//          <----------------------------    lifecycle changes since the baseline, Active
//   Active; full-state Snapshots follow at snapshot_rate
//
// The host is Active as soon as it listens. Replication (docs/Networking.md,
// "Replication") is the host's: it gives every network object a network ID, owner slot and
// incarnation, sends lifecycle changes (Spawn, Despawn, Ownership) reliably in order, each
// with the next control serial, and state in unreliable Snapshots that say which serial
// they need. Authored objects (NetworkObject in the scene) are bound by key, whatever
// their local entity; spawned ones come only from the manifest's prefabs. A client never
// creates or moves a network object on its own: it applies what the host sends, and holds
// a snapshot that arrives before the lifecycle change it needs (at most 250 ms).
//
// Clients send intent, never outcomes (docs/Networking.md, "Input and commands"): an
// InputCommand per fixed tick, stamped with the host tick it's for, and registered
// commands such as "join this team". The host takes one input per player per tick
// (falling back to the last one, briefly, when it's missing) and checks every command
// against the registry before game code sees it, with the sender's slot from the
// connection. Ping and Pong give a client the host's clock, so its inputs arrive in time.
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

#include <Engine/Components.h>
#include <Engine/Subsystem.h>
#include <Engine/World.h>
#include <Multiplayer/NetworkComponents.h>
#include <Multiplayer/NetworkSettings.h>
#include <Multiplayer/Replication.h>
#include <Network/Messages.h>
#include <Network/OnlineServices.h>
#include <Network/Transport.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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

class ReplicationRegistry;

// A prefab a session may spawn: an entry of the manifest.
struct NetworkPrefab {
    uint16_t id = 0;
    std::string path;   // inside Content
    std::string profile;   // its root's replication profile
};

// Makes a copy of a network prefab and returns its root. The default spawns the prefab
// file (spawnPrefab in Engine/Prefab.h); tests and tools may build entities instead.
using NetworkSpawnFn = std::function<Entity(World&, const NetworkPrefab&, const Transform&)>;

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
    double synchronizeTimeoutSeconds = 30;   // Welcome to Active
    // Replication: the frozen registry the schema digest was made from (needed when the
    // scene has network objects), and the manifest's prefabs, the only ones spawn() and a
    // host's Spawn can make.
    const ReplicationRegistry* registry = nullptr;
    std::vector<NetworkPrefab> prefabs;
    NetworkSpawnFn spawnPrefab;   // null: the prefab file
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

// A command the host accepted: its registry entry, sender and checked payload.
struct ReceivedCommand {
    const CommandDesc* command = nullptr;
    uint16_t commandId = 0;
    uint16_t senderSlot = 0;   // from the connection, never from the message
    uint64_t target = 0;   // network ID, or 0 for a match command
    Entity targetEntity = kNullEntity;   // its entity here (offline: the entity given)
    uint32_t tick = 0;   // the host tick it was handled at
    ProfileValue payload;
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
    // Replication.
    uint32_t objects = 0;   // network objects this session knows
    uint32_t activePeers = 0;   // host: clients past Ready
    uint64_t spawns = 0, despawns = 0, ownershipChanges = 0, respawns = 0;
    uint64_t baselinesSent = 0, baselineBytes = 0;   // host
    uint64_t snapshotsSent = 0, snapshotPartsSent = 0, snapshotBytesSent = 0;   // host
    uint64_t recordsSent = 0, recordsLimited = 0;   // host: records the budget held back
    uint64_t controlBytesSent = 0;   // host: reliable lifecycle messages, baseline included
    uint32_t controlQueueHigh = 0;   // host: the most bytes one peer's control queue held
    uint64_t captureFailures = 0;   // host: state out of its profile's ranges; not sent
    uint64_t snapshotPartsReceived = 0, recordsApplied = 0;   // client
    uint64_t staleRecords = 0;   // client: older or duplicate, old incarnation, despawned
    uint64_t heldSnapshots = 0, expiredSnapshots = 0;   // client: waited for a control serial
    uint64_t resyncRequests = 0;   // sent (client) or received (host)
    // Input and commands.
    uint64_t inputsSent = 0, inputMessagesSent = 0;   // client
    uint64_t inputsReceived = 0;   // host: commands in Inputs messages
    uint64_t inputsLate = 0;   // host: for a tick already simulated (or before Active)
    uint64_t inputsAhead = 0;   // host: more than 32 ticks ahead; persistent ones disconnect
    uint64_t inputsUsed = 0, inputFallbacks = 0;   // host: real ones, and ticks without one
    uint64_t commandsSent = 0, commandsAccepted = 0;
    uint64_t commandsRefused = 0;   // permission, target, rate or a full queue
    uint64_t pingsSent = 0, pongsReceived = 0, pingsAnswered = 0;
    double rttMs = 0;   // client: the best recent round trip, 0 until measured
    uint32_t inputLead = 0;   // client: how many ticks ahead of the host its inputs are
};

class NetworkWorld final : public Subsystem {
public:
    // Offline: local authority, nothing to do.
    NetworkWorld();
    // Online; begin() starts it. services, and world when given, must outlive it. Without a
    // world nothing replicates (the host's baseline is empty).
    NetworkWorld(const NetworkStart& start, IOnlineServices* services, World* world = nullptr);
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
    // The end of a fixed tick, after gameplay changed the world: the host notices
    // destroyed network objects, captures state and sends Snapshots on snapshot ticks,
    // and sends queued lifecycle messages within each peer's budget.
    void endTick(uint32_t tick);
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

    // ---- Replication (game.network()) ----
    // The local player's slot: 1 for the host and offline play, a client's once welcomed.
    uint16_t localPlayer() const { return m_localSlot; }
    // This machine decides the entity's state: the host and offline play, for everything.
    bool hasAuthority(Entity) const { return authority(); }
    // The entity's owner is the local player.
    bool isLocallyControlled(Entity entity) const;
    // The network object's identity, or null for an entity that isn't one (offline too).
    const NetworkIdentity* identity(Entity entity) const;
    uint64_t networkId(Entity entity) const;   // 0 if none
    Entity entity(uint64_t networkId) const;   // kNullEntity if none
    // Client: the entity Active said this player controls, or kNullEntity.
    Entity controlledEntity() const;
    uint32_t objectCount() const;

    // Host and offline only. A copy of a manifest prefab at `at`, owned by ownerSlot (0 for
    // nobody, else 1..max_players), sent to every client. Offline it's a plain spawn.
    NetResult<Entity> spawn(uint16_t prefabId, const Transform& at, uint16_t ownerSlot = 0);
    // Destroys a network object (and its children) everywhere. Destroying one directly
    // has the same effect at the end of the tick.
    NetResult<void> despawn(Entity entity);
    // Respawn or teleport: a new incarnation with the current (or `at`) state, sent
    // reliably, so nothing from the old life is applied to the new one.
    NetResult<void> respawn(Entity entity, const Transform* at = nullptr);
    // Gives the object to another player (0: nobody), with a new incarnation.
    NetResult<void> setOwner(Entity entity, uint16_t ownerSlot);
    // A Static object changed: it goes out in the next snapshot once.
    void markChanged(Entity entity);
    // Host: sends player `slot` away with this reason (Reject, then the connection
    // closes). False if no admitted client has that slot.
    bool disconnect(uint16_t slot, ReasonCode reason);

    // ---- Input and commands ----
    // Client, once per fixed tick while Active: sends this command for the next host tick
    // it can still reach (with up to three earlier ones, in case of loss) and returns that
    // tick, or 0 when none is sent (not Active yet, or already far enough ahead). The
    // world sets target_tick; incarnation is the controlled entity's.
    uint32_t sendInput(InputCommand command);
    // Host, during a fixed tick: the input player `slot` sent for this tick. False when it
    // didn't arrive in time (or is for an old incarnation), and then out is the fallback:
    // the last one's movement and aim with fire off for two ticks, then no movement. A
    // player that never sent one gets a neutral command.
    bool takeInput(uint16_t slot, uint32_t incarnation, InputCommand& out);
    // The host tick the session is on (offline: PlaySession's tick count).
    uint32_t tick() const;

    // A registered command for the host: refused when this build doesn't know it, its
    // payload is out of range, its target isn't allowed, it's a host-only command on a
    // client, or it's over its rate. A client sends it reliably; the host and offline play
    // queue it straight for pollCommand. target is a network ID (0 for none); offline,
    // where there are no IDs, pass the entity as targetEntity instead.
    NetResult<void>
    submitCommand(uint16_t commandId, uint64_t target, const ProfileValue& payload, Entity targetEntity = kNullEntity);
    // Host and offline: the next accepted command, oldest first. Game code handles them at
    // the start of a tick, with its own rules (is the round waiting? is there room?).
    bool pollCommand(ReceivedCommand& out);
    static constexpr uint32_t kMaxQueuedCommands = 64;
    static constexpr uint32_t kMaxInputLeadTicks = 32;   // host: further ahead is refused
    static constexpr uint32_t kInputFallbackTicks = 2;
    static constexpr uint32_t kInputHistory = 64;   // host: ticks of input kept per player

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
    // Replication limits (plan 6.4, 6.5).
    static constexpr uint32_t kMaxSpawnsPerSession = 65536;   // IDs are never reused
    static constexpr double kOutboundBurstBytes = 16384;   // per peer
    static constexpr uint32_t kMaxHeldSnapshots = 32;   // client, waiting for a serial
    static constexpr double kHoldSeconds = 0.25;
    static constexpr double kResyncIntervalSeconds = 1;   // at most one a second,
    static constexpr uint32_t kMaxResyncsPerMinute = 3;   // and three a minute

    struct Impl;

private:
    void setState(NetworkState state);
    void push(const NetworkEvent& event);
    void release(ReasonCode reason);

    NetworkStart m_start;
    IOnlineServices* m_services = nullptr;
    World* m_world = nullptr;
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
