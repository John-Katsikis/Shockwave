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
// Prediction (docs/Networking.md, "Prediction"): a client moves its own PredictedCharacter
// at once with each command it sends, through PhysicsWorld::predictCharacterStep, and
// keeps the command and the predicted state for 256 ticks. When the host's state for a
// tick arrives it's compared with the prediction for that tick; a miss puts the character
// where the host says and replays the commands since (movement only: no game code, no
// events), and the jump is hidden by a short visual offset. Every other object is shown
// interpolated behind the host's clock. present() writes both as PresentationTransform,
// which renderWorld draws in place of its own blend.
//
// Combat (docs/Networking.md, "Combat and events"): after every tick the host records
// each player's capsule (RewindHistory.h), and resolveShot tests a shot against the
// capsules as the shooter saw them, at the view time its input command carries, within
// a window the host works out from that player's measured round trip. The live world
// (walls, props, bots) is tested as it is now; nothing in it moves back. The host tells
// clients what happened with registered game events, reliably and in order, once each;
// a client that joins later gets the durable state from snapshots, never old events.
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
#include <Network/NetworkCapture.h>
#include <Network/OnlineServices.h>
#include <Network/Transport.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace shockwave {
class PhysicsWorld;
struct CharacterMotionState;
}   // namespace shockwave

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
    // Record every message this session sends and receives into this open capture
    // (Network/CaptureTransport.h), AuthTicket bytes redacted. It must outlive the session.
    CaptureWriter* capture = nullptr;
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

// What a command does to a predicted character in one tick: the game's own movement
// rule, which the host's simulation must apply the same way.
struct PredictedMove {
    glm::vec3 desiredVelocity{0.0f};
    bool jump = false;
};
using PredictionMoveFn = std::function<PredictedMove(const World&, Entity, const InputCommand&)>;

// What the game says about one player's character for lag compensation: the host asks
// after every tick. False leaves the slot empty (no character, or not a target at all).
struct CombatTarget {
    uint8_t team = 0;
    bool alive = true;
    bool protectedFromHits = false;   // spawn protection: recorded, the game decides
    float eyeHeight = 1.6f;   // above the feet: where this player's shots start
};
using CombatTargetFn = std::function<bool(const World&, Entity, CombatTarget&)>;

// One shot for resolveShot. command is the input command the shot was fired with (its
// view_tick and view_fraction say when the shooter's view showed the targets); null for
// the host's own player and offline play, which see the present.
struct ShotQuery {
    Entity shooter = kNullEntity;
    glm::vec3 origin{0.0f};   // the authority's eye or muzzle, never the client's
    glm::vec3 direction{0.0f, 0.0f, -1.0f};   // unit length
    float range = 100;
    const InputCommand* command = nullptr;
};

enum class ShotRewind : uint8_t {
    Present,   // tested against the present: offline, the host's own player, no history
    Exact,   // at the view time the command claimed
    Clamped,   // the view time was just outside what's plausible: moved to the edge
    Refused,   // the view time can't be true (too old for the shooter's ping): no player hit
};
const char* shotRewindName(ShotRewind rewind);

struct ShotResult {
    Entity hit = kNullEntity;   // what the shot stopped at: a player, a world entity, or nothing
    bool player = false;   // hit is a player, tested where they were at viewTick
    glm::vec3 end{0.0f};   // where the shot stopped (origin + direction * range for a miss)
    float distance = 0;
    uint32_t hitIncarnation = 0;   // the player's life it hit
    ShotRewind rewind = ShotRewind::Present;
    uint32_t viewTick = 0;   // the time it was tested at: a tick and a fraction
    float viewFraction = 0;
};

// A game event the host sent (or, on the host and offline, made): its registry entry,
// the tick it happened at, its subject and checked payload. sequence numbers every event
// of the session from 1, the same on every peer, so readers can tell what they've seen.
struct ReceivedGameEvent {
    const EventDesc* event = nullptr;   // null offline without a registry
    uint16_t eventId = 0;
    uint64_t sequence = 0;
    uint32_t tick = 0;
    uint64_t subject = 0;   // network ID, or 0 for a match event
    Entity subjectEntity = kNullEntity;   // its entity here, when it still exists
    ProfileValue payload;
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
    uint64_t inputsAhead = 0;   // host: more than kMaxInputLeadTicks ahead; persistent ones disconnect
    uint64_t inputsUsed = 0, inputFallbacks = 0;   // host: real ones, and ticks without one
    uint64_t commandsSent = 0, commandsAccepted = 0;
    uint64_t commandsRefused = 0;   // permission, target, rate or a full queue
    uint64_t pingsSent = 0, pongsReceived = 0, pingsAnswered = 0;
    double rttMs = 0;   // client: the best recent round trip, 0 until measured (host: its clients' worst)
    uint32_t inputLead = 0;   // client: how many ticks ahead of the host its inputs are
    // Prediction (client).
    uint64_t predictedTicks = 0;   // commands predicted when sent
    uint64_t catchUpInputs = 0;   // extra commands sent because the clock moved ahead
    uint64_t predictionChecks = 0, predictionMatches = 0;   // host states compared, and within tolerance
    uint64_t corrections = 0, replayedTicks = 0;
    uint64_t predictionResets = 0;   // snapped: no history for the tick, or too far to replay
    float lastCorrection = 0, maxCorrection = 0;   // metres
    uint32_t interpolatedObjects = 0;   // shown interpolated by the last present()
    // Combat and events.
    uint64_t shotsResolved = 0, playerHits = 0, worldHits = 0;   // authority
    uint64_t shotsRewound = 0, shotsClamped = 0, shotsRefused = 0;   // host: by ShotRewind
    uint64_t gameEventsSent = 0;   // host: events made (each goes to every Active client)
    uint64_t gameEventsReceived = 0, duplicateEvents = 0;   // client
    uint64_t gameEventsMissed = 0;   // readers that fell more than kGameEventHistory behind
    // Cost: time in pump, drain, prepareStep, endTick and present, and the part of it a
    // client spent replaying predicted movement (physics). Nanoseconds, from the start.
    uint64_t workNanos = 0, replayNanos = 0;
};

// One remote machine as the Network window shows it: on the host each connection, on a
// client the host. Ages are seconds, -1 until there's been one.
struct NetworkPeerInfo {
    uint16_t slot = 0;   // 0 while still in the handshake
    PeerId peer;
    const char* status = "";   // connecting, authenticating, synchronizing, active
    double rttMs = 0;   // 0 until measured
    uint32_t pendingReliableBytes = 0, pendingUnreliableBytes = 0;   // the transport's queues
    uint32_t queuedControlBytes = 0;   // host: lifecycle messages waiting for the peer's budget
    uint64_t bytesReceived = 0, bytesSent = 0;   // over the connection so far
    double inputAge = -1;   // host: since this player's newest input arrived
    double snapshotAge = -1;   // client: since a snapshot was last applied
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
    // Heap bytes the session's own buffers hold: its state, every peer's queues and
    // histories, objects, interpolation and rewind. Not the transport's (Steam's or the
    // hub's), the World's entities or physics.
    size_t memoryBytes() const;
    // Every connected peer, by slot (handshakes last). Fills out; allocates only if out is
    // too small.
    void peerInfo(std::vector<NetworkPeerInfo>& out) const;
    // The session's transport, for its counters (a capture's, say); null offline.
    const ITransport* transport() const { return m_transport.get(); }

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
    // Limits given in ticks at 60 Hz are times: at tick_rate 120 they're twice as many
    // ticks (scaledTicks), so a faster tick doesn't shrink the latency a session copes with.
    static constexpr uint32_t scaledTicks(uint32_t ticksAt60, uint32_t tickRate) {
        return ticksAt60 * (tickRate > 60 ? tickRate : 60) / 60;
    }
    static constexpr uint32_t kMaxInputLeadTicks = 32;   // at 60 Hz; host: further ahead is refused
    static constexpr uint32_t kInputFallbackTicks = 2;
    static constexpr uint32_t kInputHistory = 128;   // host: ticks of input kept per player (> 2 x 32 at 120 Hz)

    // ---- Combat history and game events ----
    // How the game describes its players' characters to lag compensation. Without one
    // nothing is recorded, and resolveShot tests the present.
    void setCombatTargets(CombatTargetFn targets) { m_combatTargets = std::move(targets); }
    // Authority only (false on a client): the nearest thing the shot hits. Players are
    // tested where the shooter saw them (from the history, at the command's view time,
    // checked against the shooter's measured round trip); everything else, and anything
    // in front of the player, as it is now. Offline, and for the host's own player, it's
    // the present.
    bool resolveShot(const ShotQuery& shot, ShotResult& out);
    // Host and offline: a registered event, now. The host sends it reliably to every
    // Active client; it's also readable here (readGameEvent). Refused when it isn't
    // registered, its payload is out of range, or this is a client.
    NetResult<void> sendEvent(uint16_t eventId, Entity subject, const ProfileValue& payload);
    // Game events, oldest first, each once per reader: cursor starts at gameEventCursor()
    // and moves past each event read. Up to kGameEventHistory are kept; a reader further
    // behind skips ahead (counted in gameEventsMissed).
    bool readGameEvent(uint64_t& cursor, ReceivedGameEvent& out) const;
    uint64_t gameEventCursor() const;   // where a new reader starts: after every event so far
    static constexpr uint32_t kGameEventHistory = 128;
    // The same for session events (joins, leaves, state changes), for readers besides
    // the game's pollEvent, such as circuits.
    bool readEvent(uint64_t& cursor, NetworkEvent& out) const;
    uint64_t eventCursor() const { return m_eventSequence; }
    // Host: a client's measured round trip in seconds (the worst of its recent pings),
    // 0 until measured. Client: the best recent round trip to the host.
    double roundTrip(uint16_t slot) const;
    static constexpr double kHostPingSeconds = 1;   // the host pings each Active client
    static constexpr uint32_t kViewToleranceTicks = 2;   // at 60 Hz: slack on a shot's view time

    // ---- Prediction and presentation ----
    // The session's physics: sets its network mode from the role (Offline, Authority,
    // Client) and lets a client predict. PlaySession calls it before beginPlay.
    void setPhysics(PhysicsWorld* physics);
    // The game's movement rule for predicted characters. Without one nothing is predicted:
    // the client's character waits for the host's state like any other object.
    void setPredictionMove(PredictionMoveFn move) { m_predictionMove = std::move(move); }
    // A client moving its controlled character ahead of the host.
    bool predicting() const;
    // Before every physics step: tells physics which characters are predicted (the host:
    // every owned PredictedCharacter; a client: its own).
    void prepareStep();
    // Once per frame before the game's frame(): writes PresentationTransform for the
    // controlled character (blended between ticks, plus the correction offset) and for
    // interpolated objects. alpha is the engine's interpolation alpha.
    void present(float alpha);
    // The visual offset still being hidden after a correction, now.
    glm::vec3 correctionOffset() const;
    // Set when prediction had to snap; cleared after kWarningSeconds.
    const std::string& predictionWarning() const;
    static constexpr uint32_t kPredictionHistory = 256;   // ticks of commands and predicted states
    static constexpr uint32_t kMaxReplayTicks = 32;   // at 60 Hz; further back than this snaps instead
    static constexpr uint32_t kMaxCatchUpInputs = 4;   // one Inputs message
    static constexpr float kPositionTolerance = 0.02f;   // metres
    static constexpr float kVelocityTolerance = 0.05f;   // metres per second
    static constexpr float kSnapDistance = 1.0f;   // a bigger correction isn't smoothed
    static constexpr double kOffsetHalfLife = 0.05, kOffsetMaxSeconds = 0.15;
    static constexpr double kWarningSeconds = 3;
    static constexpr uint32_t kMaxInterpolatedObjects = 512;

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
    PhysicsWorld* m_physics = nullptr;
    PredictionMoveFn m_predictionMove;
    CombatTargetFn m_combatTargets;
    // Events by sequence (event n is at n % kMaxEvents); pollEvent reads from m_pollCursor.
    std::array<NetworkEvent, kMaxEvents> m_events{};
    uint64_t m_eventSequence = 0, m_pollCursor = 0;
};

}   // namespace shockwave::net
