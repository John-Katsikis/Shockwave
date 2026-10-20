#pragma once

// NetworkWorld's private state, shared by NetworkWorld.cpp (roles, the handshake,
// shutdown) and NetworkReplication.cpp (baseline, lifecycle, snapshots, budgets).

#include <Multiplayer/NetworkWorld.h>
#include <Multiplayer/Replication.h>

#include <Network/Sha256.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace shockwave::net {

// One remote machine: on the host, each connection; on a client, the host.
struct Peer {
    enum class Phase : uint8_t { Free, AwaitingHello, Authenticating, Synchronizing, Active };
    Phase phase = Phase::Free;
    ConnectionId connection;
    PeerId peer;
    bool connected = false;   // the transport said Connected (a client's view of the host)
    double since = -1;   // when the phase began; -1 until the first pump gives a time
    uint16_t slot = 0;
    RequestId ticketRequest;   // our ticket for this peer
    bool ticketSent = false;
    bool authBegun = false;   // we're validating its ticket
    bool validated = false;   // and it was good
    double tokens = NetworkWorld::kPreAuthBurstBytes;   // pre-auth receive budget
    double tokensAt = -1;

    // Host replication, once welcomed. Reliable messages wait in `queue` (each a u32
    // length, then the encoded message) and go out in order within the peer's budget.
    std::vector<uint8_t> queue;
    size_t queueRead = 0;
    size_t queueCap = 0;
    uint32_t baselineId = 0;
    Digest baselineDigest{};
    double outTokens = NetworkWorld::kOutboundBurstBytes;
    double outTokensAt = -1;
    uint32_t sentSerial = 0;   // the newest control serial handed to the transport
    uint32_t propCursor = 0;   // round-robin position among non-player objects
    bool fullRefresh = false;   // the next snapshot carries every object
    std::array<double, NetworkWorld::kMaxResyncsPerMinute> resyncs{-1e9, -1e9, -1e9};

    // Host input, once Active: a ring of commands by target tick, the last one used (for
    // the fallback), and how often it sent inputs too far ahead.
    std::array<InputCommand, NetworkWorld::kInputHistory> inputs{};
    std::array<bool, NetworkWorld::kInputHistory> haveInput{};
    uint32_t firstInputTick = 0;
    uint32_t lastTaken = 0;   // the newest tick takeInput() used, real or fallback
    bool tookAny = false;
    InputCommand lastInput{};
    bool haveLastInput = false;
    uint32_t missing = 0;
    uint32_t aheadViolations = 0;
    double aheadWindow = -1;
    // Host commands: the last command_seq, and a token bucket per registered command.
    uint32_t commandSeq = 0;
    bool haveCommandSeq = false;
    std::vector<double> commandTokens, commandTokensAt;
    // Pings answered this second.
    uint32_t pings = 0;
    double pingWindow = -1;

    bool admitted() const { return phase == Phase::Synchronizing || phase == Phase::Active; }
    size_t queued() const { return queue.size() - queueRead; }
};

// A network object: on the host, one it replicates; on a client, one the host told it
// about (staged until the baseline is committed, then bound to an entity).
struct NetObject {
    uint64_t id = 0;
    uint32_t incarnation = 0;
    uint16_t owner = 0;
    uint16_t prefabId = 0;   // 0: authored, bound by key
    std::string key;
    const ReplicationProfile* profile = nullptr;
    Entity entity = kNullEntity;
    bool isStatic = false;
    bool changed = false;   // host: markChanged()
    uint32_t serial = 0;   // the control serial of its last lifecycle change
    uint32_t lastTick = 0;   // client: newest snapshot tick applied
    bool haveTick = false;
    // The last valid encoded state: the host's newest capture, or what a client was sent.
    uint16_t stateBytes = 0;
    std::array<uint8_t, kMaxStateBytes> state{};
};

// A client's snapshot that arrived before the lifecycle change it needs.
struct HeldSnapshot {
    bool used = false;
    double at = 0;
    uint32_t requiredSerial = 0;
    uint32_t bytes = 0;
    std::array<uint8_t, kMaxUnreliableMessageBytes> data{};
};

// u32 serial and tick order that survives wraparound.
inline bool newer(uint32_t a, uint32_t b) { return a != b && uint32_t(a - b) < 0x80000000u; }

struct NetworkWorld::Impl final : TransportSink {
    explicit Impl(NetworkWorld& world);

    NetworkWorld& w;
    std::array<Peer, kMaxTransportConnections> peers{};   // host: one per connection; client: [0] is the host
    std::array<uint8_t, kMaxReliableMessageBytes> out{};
    Nonce session{};   // host: made at begin(); client: learned from Challenge
    Nonce clientNonce{};
    bool challenged = false, welcomed = false;   // client
    double now = 0;
    bool haveClock = false;
    uint32_t tick = 0;
    uint32_t endpointGeneration = 0;
    bool discarding = false;   // close(): drain and drop
    bool released = false;
    // A failure found inside poll() waits for it to return: the transport can't be
    // destroyed from its own callback.
    bool polling = false, failPending = false;
    NetworkWorldStats stats;

    // ---- Replication state ----
    std::vector<NetObject> objects;   // by ascending network ID
    std::unordered_map<uint64_t, uint32_t> byId;   // network ID -> index into objects
    uint64_t nextId = 1;   // host
    uint64_t highestId = 0;   // client: IDs up to this one were spawned at some point
    uint32_t serial = 0;   // the newest control serial sent (host) or applied (client)
    uint32_t snapshotId = 0;   // host
    std::unordered_map<std::string, Entity> localKeys;   // client: authored roots by key
    // Client baseline: begun with BaselineBegin, ended (digest checked) by BaselineEnd,
    // committed into the world once the host is also validated.
    uint32_t baselineId = 0;
    uint16_t baselineCount = 0, maxEntities = kMaxNetworkObjects;
    bool baselineBegun = false, baselineEnded = false, committed = false, activated = false;
    Digest baselineDigest{};
    uint64_t controlled = 0;   // client: Active's controlled_entity
    std::array<HeldSnapshot, kMaxHeldSnapshots> held{};
    uint32_t lastAppliedTick = 0;   // client: newest snapshot tick applied to anything
    bool haveApplied = false;
    double lastResync = -1e9;
    std::array<double, kMaxResyncsPerMinute> resyncTimes{-1e9, -1e9, -1e9};
    // Host scratch, sized once: records picked for a peer, part boundaries, a part.
    std::vector<uint32_t> pick, partEnds;
    std::array<uint8_t, kMaxUnreliableMessageBytes> part{};
    std::array<uint8_t, kMaxReliableMessageBytes> controlOut{};   // lifecycle messages being queued
    bool captureWarned = false;

    // ---- Input and commands (NetworkInput.cpp) ----
    // Client clock: Pong samples (the host tick when it arrived, the round trip), the best
    // of the last eight used. Inputs: the last target tick sent and the last four sent.
    struct ClockSample {
        double hostTick = 0;   // estimated host tick at `at`
        double at = 0;
        double rtt = 0;
    };
    std::array<ClockSample, 8> clock{};
    uint32_t clockSamples = 0, clockNext = 0;
    uint32_t pingId = 0, pingsSent = 0;
    double nextPing = 0;
    uint32_t firstInputTick = 0;   // client: Active's first_input_tick
    uint32_t lastInputTick = 0;
    bool sentInput = false;
    std::array<InputCommand, kMaxInputsPerMessage> recentInputs{};
    uint32_t recentCount = 0;
    uint32_t clientCommandSeq = 0;
    std::vector<double> clientTokens, clientTokensAt;   // client: the same rate limits
    // Accepted commands waiting for pollCommand.
    std::array<ReceivedCommand, NetworkWorld::kMaxQueuedCommands> commands{};
    uint32_t commandHead = 0, commandCount = 0;
    std::string beginError;   // what a NetError from beginReplication() points at

    const NetworkSettings& settings() const { return w.m_start.settings; }
    bool host() const { return w.m_role == NetworkRole::Authority; }
    uint32_t maxClients() const;
    Peer* find(ConnectionId connection);
    uint32_t count(Peer::Phase phase) const;
    uint32_t admittedCount() const;
    uint32_t used() const;
    void enter(Peer& p, Peer::Phase phase);
    bool send(ConnectionId connection, const MessageBody& body);
    void drop(Peer& p, ReasonCode reason);
    void reject(Peer& p, ReasonCode reason, std::string_view diagnostic);
    void failClient(ReasonCode reason, const char* why) { w.fail(reason, why); }
    void admit(Peer& p);
    void maybeAdmit(Peer& p);
    void maybeSynchronized();

    // Transport (NetworkWorld.cpp).
    void poll();
    void finishFail();
    void onEvent(const TransportEvent& event) override;
    void hostEvent(const TransportEvent& event);
    void clientEvent(const TransportEvent& event);
    void onMessage(const ReceivedMessage& message) override;
    void hostMessage(Peer& p, const ReceivedMessage& message);
    void hello(Peer& p, const Hello& hello);
    void clientMessage(Peer& h, const ReceivedMessage& message);
    void challenge(Peer& h, const MessageHeader& header, const Challenge& c);
    void checkTimeouts();

    // ---- Replication (NetworkReplication.cpp) ----
    NetObject* object(uint64_t id);
    NetObject* objectOf(Entity entity);
    void insert(NetObject object);
    void erase(uint64_t id);
    void writeIdentity(const NetObject& o);   // the entity's NetworkIdentity
    // begin(): the host numbers the scene's network objects, a client indexes them by key.
    NetResult<void> beginReplication();
    void clearReplication();
    // Host.
    bool captureState(NetObject& o);
    void encodeSpawn(const NetObject& o, Spawn& spawn) const;
    bool append(Peer& p, std::span<const uint8_t> bytes, uint32_t controlSerial);
    bool enqueue(Peer& p, const MessageBody& body);
    void broadcast(const MessageBody& body);
    void startBaseline(Peer& p);
    void ready(Peer& p, const Ready& ready);
    void resyncRequest(Peer& p);
    void changeOwner(NetObject& o, uint16_t slot);
    void ownerLeft(uint16_t slot);
    void despawnObject(uint64_t id);
    void flushControl(Peer& p);
    void flushAll();
    void sweepDestroyed();
    void sendSnapshots();
    void sendSnapshot(Peer& p);
    // Client. replicationMessage is false for a message that isn't replication's.
    bool replicationMessage(const DecodedMessage& decoded);
    bool spawnMessage(const Spawn& spawn);
    bool checkSerial(uint32_t controlSerial);
    bool validState(const ReplicationProfile& profile, std::span<const uint8_t> bytes);
    void tryCommit();
    bool commitBaseline();
    bool materialize(NetObject& o);
    void applyState(NetObject& o);
    bool applySnapshot(const Snapshot& snapshot);
    void holdSnapshot(std::span<const uint8_t> bytes, uint32_t requiredSerial);
    void releaseHeld();
    void expireHeld();
    void requestResync(uint8_t reason);
    Digest catalogDigest() const;

    // ---- Input and commands (NetworkInput.cpp) ----
    Peer* peerForSlot(uint16_t slot);
    void inputsMessage(Peer& p, const Inputs& inputs);
    void commandMessage(Peer& p, const Command& command);
    void pingMessage(Peer& p, const Ping& ping);
    void pongMessage(const Pong& pong);
    void sendPings();
    // Whether the command may be taken from this sender: permission, target, rate. why
    // says why not.
    bool commandAllowed(
        const CommandDesc& desc, uint16_t sender, uint64_t target, Entity& targetEntity, std::vector<double>& tokens,
        std::vector<double>& tokensAt, const char*& why);
    bool queueCommand(
        const CommandDesc& desc, uint16_t sender, uint64_t target, Entity targetEntity, const ProfileValue& payload);
    bool estimatedHostTick(double& out) const;
    void clearInput();
};

}   // namespace shockwave::net
