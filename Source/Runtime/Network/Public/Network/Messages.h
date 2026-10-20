#pragma once

// Wire protocol v1: the 32-byte message header, the catalog of message kinds and their
// payloads, input commands, state records and the built-in pose state.
//
// Each message is a plain struct whose fields() lists its wire fields in order, with
// their types and allowed ranges. That one list drives the encoder, the decoder, packet
// dumps (NetworkCapture.h) and the generated reference, docs/NetworkProtocol.md, so they
// can't disagree. Decoding checks everything (lengths, counts, ranges, UTF-8, finite
// floats, delivery and direction) into a temporary DecodedMessage; nothing is applied
// unless the whole message is valid. Strings and byte fields in a decoded message point
// into the bytes it was decoded from.
//
// Neither encoding nor decoding allocates.

#include <Network/NetworkTypes.h>
#include <Network/Transport.h>
#include <Network/Wire.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace shockwave::net {

inline constexpr uint16_t kProtocolVersion = 1;
inline constexpr uint32_t kHeaderBytes = 32;
inline constexpr std::array<uint8_t, 4> kMagic = {'S', 'W', 'M', 'P'};

// Hard limits, whatever the project's settings say.
inline constexpr uint32_t kMaxReliableMessageBytes = 16384;   // including the header
inline constexpr uint32_t kMaxUnreliableMessageBytes = 1152;   // including the header
inline constexpr uint16_t kMaxCommandPayloadBytes = 256;
inline constexpr uint16_t kMaxEventPayloadBytes = 512;
inline constexpr uint16_t kMaxStateBytes = 512;   // one state record's payload
inline constexpr uint32_t kMaxBaselineBytes = 4u << 20;
inline constexpr uint16_t kMaxNetworkObjects = 4096;
inline constexpr uint16_t kMaxAuthTicketBytes = 4096;
inline constexpr uint16_t kMaxGameProtocolBytes = 64;
inline constexpr uint16_t kMaxAuthoredKeyBytes = 96;
inline constexpr uint16_t kMaxDiagnosticBytes = 160;
inline constexpr uint8_t kMaxInputsPerMessage = 4;
inline constexpr uint32_t kInputCommandBytes = 24;
inline constexpr uint32_t kStateRecordHeaderBytes = 16;
inline constexpr uint32_t kPoseStateBytes = 53;
inline constexpr uint16_t kMaxPlayers = 64;   // slot 1 is the host; 2..64 are remote players
// Delta snapshots: entities are numbered by slot, 0..kMaxDeltaSlots-1, in a table both
// sides agree on through reliable lifecycle messages.
inline constexpr uint16_t kMaxDeltaSlots = 1024;
// Simulation tick rates protocol v1 accepts. A session's rate must also equal the
// project's tick_rate, and its snapshot rate must divide it.
inline constexpr uint16_t kMinTickRate = 60;
inline constexpr uint16_t kMaxTickRate = 120;
inline constexpr std::array<uint16_t, 6> kSnapshotRates = {10, 15, 20, 30, 60, 120};

using Nonce = std::array<uint8_t, 16>;
using Digest = std::array<uint8_t, 32>;

struct NetVec3 {
    float x = 0, y = 0, z = 0;
    friend bool operator==(const NetVec3&, const NetVec3&) = default;
};
struct NetQuat {
    float x = 0, y = 0, z = 0, w = 1;
    friend bool operator==(const NetQuat&, const NetQuat&) = default;
};

enum class MessageKind : uint8_t {
    Hello = 1,
    Challenge = 2,
    AuthTicket = 3,
    Welcome = 4,
    BaselineBegin = 5,
    Spawn = 6,
    Despawn = 7,
    Ownership = 8,
    InitialState = 9,
    BaselineEnd = 10,
    Ready = 11,
    Active = 12,
    Inputs = 13,
    Snapshot = 14,
    Command = 15,
    GameEvent = 16,
    Ping = 17,
    Pong = 18,
    Reject = 19,
    Goodbye = 20,
    ResyncRequest = 21,
    DeltaSnapshot = 22,
    SnapshotAck = 23,
};
inline constexpr uint8_t kLastMessageKind = 23;

enum class PeerRole : uint8_t { Host, Client };
enum class MessageDirection : uint8_t { ClientToHost, HostToClient, Either };

struct MessageDescriptor {
    MessageKind kind;
    const char* name;
    Delivery delivery;   // fixed by the catalog; a sender can't choose
    MessageDirection direction;
    const char* when;   // the session phase it belongs to
};

std::span<const MessageDescriptor> messageCatalog();
// nullptr for a kind protocol v1 doesn't have.
const MessageDescriptor* findMessage(uint8_t kind);
inline uint32_t maxMessageBytes(Delivery delivery) {
    return delivery == Delivery::Unreliable ? kMaxUnreliableMessageBytes : kMaxReliableMessageBytes;
}

enum class WireStatusCode : uint8_t {
    Ok,
    Truncated,
    BadMagic,
    BadVersion,
    UnknownKind,
    BadFlags,
    LengthMismatch,   // the header's payload length isn't the bytes received
    TooLarge,
    WrongDelivery,
    WrongDirection,
    OutOfRange,
    NonFinite,
    BadString,
    TrailingBytes,
    Invalid,   // fields that are fine alone but not together
    Overflow,   // encoding: the output buffer is too small
};

const char* wireStatusName(WireStatusCode code);

struct [[nodiscard]] WireStatus {
    WireStatusCode code = WireStatusCode::Ok;
    uint32_t offset = 0;   // where in the message it went wrong
    const char* field = "";   // which field, when one did
    explicit operator bool() const { return code == WireStatusCode::Ok; }
    // How a session reports it to the peer: an unknown kind or version is a protocol
    // mismatch, anything else a malformed packet.
    ReasonCode reason() const;
};

struct MessageHeader {
    MessageKind kind = MessageKind::Hello;
    uint8_t flags = 0;   // none are defined in v1; any set bit is rejected
    Nonce session{};
    uint32_t senderTick = 0;
    uint32_t payloadBytes = 0;
};

// Input command (24 bytes), sent in Inputs.
struct InputCommand {
    uint32_t targetTick = 0;   // host tick it's for; also its sequence number
    uint32_t incarnation = 0;
    int8_t moveX = 0, moveZ = 0;   // -127..127, diagonal no longer than 128
    uint16_t yaw = 0;   // [0, 65535] maps to [-pi, pi)
    int16_t pitch = 0;   // [-32767, 32767] maps to [-89, 89] degrees
    uint16_t heldBits = 0;   // bit 0 fire, bit 1 sprint
    uint16_t pressedBits = 0;   // bit 0 jump, bit 1 reload
    uint32_t viewTick = 0;   // remote interpolation time shown when sampled
    uint16_t viewFraction = 0;   // fraction of a tick, /65536

    static constexpr uint16_t kHeldFire = 1, kHeldSprint = 2;
    static constexpr uint16_t kPressedJump = 1, kPressedReload = 2;

    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.u32("target_tick", m.targetTick);
        v.u32("incarnation", m.incarnation);
        v.i8("move_x", m.moveX, -127, 127);
        v.i8("move_z", m.moveZ, -127, 127);
        v.u16("yaw", m.yaw);
        v.i16("pitch", m.pitch, -32767, 32767);
        v.bits16("held_bits", m.heldBits, kHeldFire | kHeldSprint);
        v.bits16("pressed_bits", m.pressedBits, kPressedJump | kPressedReload);
        v.u32("view_tick", m.viewTick);
        v.u16("view_fraction", m.viewFraction);
    }
    const char* invalid() const;
    friend bool operator==(const InputCommand&, const InputCommand&) = default;
};

uint16_t encodeYaw(float radians);   // wraps into [-pi, pi)
float decodeYaw(uint16_t yaw);
int16_t encodePitch(float radians);   // clamps to +-89 degrees
float decodePitch(int16_t pitch);

// Messages, in catalog order. `invalid()` returns the first field that breaks a rule
// involving more than one field, or nullptr.

struct Hello {
    static constexpr MessageKind kKind = MessageKind::Hello;
    std::string_view gameProtocol;
    Digest schemaDigest{}, contentDigest{};
    uint16_t sceneId = 0;
    uint64_t lobbyId = 0;   // 0 for a developer direct join
    Nonce clientNonce{};
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.string("game_protocol", m.gameProtocol, 1, kMaxGameProtocolBytes);
        v.digest("schema_digest", m.schemaDigest);
        v.digest("content_digest", m.contentDigest);
        v.u16("scene_id", m.sceneId, 1);
        v.u64("lobby_id", m.lobbyId);
        v.nonce("client_nonce", m.clientNonce);
    }
    const char* invalid() const { return nullptr; }
};

struct Challenge {
    static constexpr MessageKind kKind = MessageKind::Challenge;
    Nonce clientNonce{};
    std::string_view gameProtocol;
    Digest schemaDigest{}, contentDigest{};
    uint16_t sceneId = 0;
    uint64_t hostPeer = 0;
    uint16_t tickRate = 60;
    uint16_t snapshotRate = 20;
    uint16_t interpolationMs = 100;
    uint16_t maxRewindMs = 200;
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.nonce("client_nonce", m.clientNonce);
        v.string("game_protocol", m.gameProtocol, 1, kMaxGameProtocolBytes);
        v.digest("schema_digest", m.schemaDigest);
        v.digest("content_digest", m.contentDigest);
        v.u16("scene_id", m.sceneId, 1);
        v.u64("host_peer", m.hostPeer, 1);
        v.u16("tick_rate", m.tickRate, kMinTickRate, kMaxTickRate);
        v.u16("snapshot_rate", m.snapshotRate, kSnapshotRates.front(), kSnapshotRates.back());
        v.u16("interpolation_ms", m.interpolationMs, 25, 200);
        v.u16("max_rewind_ms", m.maxRewindMs, 0, 250);
    }
    // snapshot_rate must be one of kSnapshotRates and divide tick_rate.
    const char* invalid() const;
};

struct AuthTicket {
    static constexpr MessageKind kKind = MessageKind::AuthTicket;
    std::span<const uint8_t> ticket;   // never logged or captured
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.bytes("ticket", m.ticket, 1, kMaxAuthTicketBytes);
    }
    const char* invalid() const { return nullptr; }
};

struct Welcome {
    static constexpr MessageKind kKind = MessageKind::Welcome;
    uint16_t playerSlot = 2;
    uint32_t baselineId = 1;
    uint32_t serverTick = 0;
    uint16_t maxEntities = 512;
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.u16("player_slot", m.playerSlot, 2, kMaxPlayers);
        v.u32("baseline_id", m.baselineId, 1);
        v.u32("server_tick", m.serverTick);
        v.u16("max_entities", m.maxEntities, 12, kMaxNetworkObjects);
    }
    const char* invalid() const { return nullptr; }
};

struct BaselineBegin {
    static constexpr MessageKind kKind = MessageKind::BaselineBegin;
    uint32_t baselineId = 1;
    uint32_t baselineTick = 0;
    uint16_t objectCount = 0;
    uint32_t totalStateBytes = 0;
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.u32("baseline_id", m.baselineId, 1);
        v.u32("baseline_tick", m.baselineTick);
        v.u16("object_count", m.objectCount, 0, kMaxNetworkObjects);
        v.u32("total_state_bytes", m.totalStateBytes, 0, kMaxBaselineBytes);
    }
    const char* invalid() const { return nullptr; }
};

struct Spawn {
    static constexpr MessageKind kKind = MessageKind::Spawn;
    uint32_t controlSerial = 1;
    uint64_t entity = 0;
    uint32_t incarnation = 0;
    uint16_t ownerSlot = 0;   // 0: no player owns it
    uint16_t prefabId = 0;   // 0: an authored object, bound by authored_key
    std::string_view authoredKey;
    uint16_t profileId = 0;
    std::span<const uint8_t> initialState;
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.u32("control_serial", m.controlSerial, 1);
        v.u64("entity", m.entity, 1);
        v.u32("incarnation", m.incarnation);
        v.u16("owner_slot", m.ownerSlot, 0, kMaxPlayers);
        v.u16("prefab_id", m.prefabId);
        v.string("authored_key", m.authoredKey, 0, kMaxAuthoredKeyBytes);
        v.u16("profile_id", m.profileId, 1, 1023);
        v.bytes("initial_state", m.initialState, 0, kMaxStateBytes);
    }
    // An authored object (prefab 0) needs a key; a spawned prefab has none.
    const char* invalid() const;
};

struct Despawn {
    static constexpr MessageKind kKind = MessageKind::Despawn;
    uint32_t controlSerial = 1;
    uint64_t entity = 0;
    uint8_t reason = 0;
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.u32("control_serial", m.controlSerial, 1);
        v.u64("entity", m.entity, 1);
        v.u8("reason", m.reason);
    }
    const char* invalid() const { return nullptr; }
};

struct Ownership {
    static constexpr MessageKind kKind = MessageKind::Ownership;
    uint32_t controlSerial = 1;
    uint64_t entity = 0;
    uint32_t incarnation = 0;
    uint16_t ownerSlot = 0;
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.u32("control_serial", m.controlSerial, 1);
        v.u64("entity", m.entity, 1);
        v.u32("incarnation", m.incarnation);
        v.u16("owner_slot", m.ownerSlot, 0, kMaxPlayers);
    }
    const char* invalid() const { return nullptr; }
};

// `records` holds record_count state records back to back, as writeStateRecord() makes
// them and StateRecordReader reads them.
struct InitialState {
    static constexpr MessageKind kKind = MessageKind::InitialState;
    uint32_t baselineId = 1;
    uint16_t recordCount = 0;
    std::span<const uint8_t> records;
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.u32("baseline_id", m.baselineId, 1);
        v.records("records", m.recordCount, m.records);
    }
    const char* invalid() const { return nullptr; }
};

struct BaselineEnd {
    static constexpr MessageKind kKind = MessageKind::BaselineEnd;
    uint32_t baselineId = 1;
    uint32_t finalControlSerial = 0;
    Digest baselineDigest{};
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.u32("baseline_id", m.baselineId, 1);
        v.u32("final_control_serial", m.finalControlSerial);
        v.digest("baseline_digest", m.baselineDigest);
    }
    const char* invalid() const { return nullptr; }
};

struct Ready {
    static constexpr MessageKind kKind = MessageKind::Ready;
    uint32_t baselineId = 1;
    Digest baselineDigest{};
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.u32("baseline_id", m.baselineId, 1);
        v.digest("baseline_digest", m.baselineDigest);
    }
    const char* invalid() const { return nullptr; }
};

struct Active {
    static constexpr MessageKind kKind = MessageKind::Active;
    uint32_t startTick = 0;
    uint32_t firstInputTick = 0;
    uint64_t controlledEntity = 0;   // 0: nothing to control
    uint32_t incarnation = 0;
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.u32("start_tick", m.startTick);
        v.u32("first_input_tick", m.firstInputTick);
        v.u64("controlled_entity", m.controlledEntity);
        v.u32("incarnation", m.incarnation);
    }
    const char* invalid() const { return nullptr; }
};

// The newest command plus up to three earlier unacknowledged ones, oldest first.
struct Inputs {
    static constexpr MessageKind kKind = MessageKind::Inputs;
    uint8_t count = 1;
    std::array<InputCommand, kMaxInputsPerMessage> commands{};
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.inputs("commands", m.count, m.commands);
    }
    const char* invalid() const { return nullptr; }
};

struct Snapshot {
    static constexpr MessageKind kKind = MessageKind::Snapshot;
    uint32_t snapshotId = 0;
    uint32_t serverTick = 0;
    uint32_t requiredControlSerial = 0;
    uint16_t partIndex = 0;
    uint16_t partCount = 1;
    uint16_t recordCount = 0;
    std::span<const uint8_t> records;
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.u32("snapshot_id", m.snapshotId);
        v.u32("server_tick", m.serverTick);
        v.u32("required_control_serial", m.requiredControlSerial);
        v.u16("part_index", m.partIndex, 0, 255);
        v.u16("part_count", m.partCount, 1, 256);
        v.records("records", m.recordCount, m.records);
    }
    // part_index < part_count.
    const char* invalid() const;
};

struct Command {
    static constexpr MessageKind kKind = MessageKind::Command;
    uint32_t commandSeq = 0;
    uint64_t target = 0;   // network entity, or 0
    uint16_t commandId = 0;
    std::span<const uint8_t> payload;
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.u32("command_seq", m.commandSeq);
        v.u64("target", m.target);
        v.u16("command_id", m.commandId, 1);
        v.bytes("payload", m.payload, 0, kMaxCommandPayloadBytes);
    }
    const char* invalid() const { return nullptr; }
};

struct GameEvent {
    static constexpr MessageKind kKind = MessageKind::GameEvent;
    uint32_t eventSeq = 0;
    uint32_t eventTick = 0;
    uint64_t subject = 0;   // network entity, or 0 for a match event
    uint16_t eventId = 0;
    std::span<const uint8_t> payload;
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.u32("event_seq", m.eventSeq);
        v.u32("event_tick", m.eventTick);
        v.u64("subject", m.subject);
        v.u16("event_id", m.eventId, 1);
        v.bytes("payload", m.payload, 0, kMaxEventPayloadBytes);
    }
    const char* invalid() const { return nullptr; }
};

struct Ping {
    static constexpr MessageKind kKind = MessageKind::Ping;
    uint32_t pingId = 0;
    uint64_t originUs = 0;
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.u32("ping_id", m.pingId);
        v.u64("origin_monotonic_us", m.originUs);
    }
    const char* invalid() const { return nullptr; }
};

struct Pong {
    static constexpr MessageKind kKind = MessageKind::Pong;
    uint32_t pingId = 0;
    uint64_t echoedOriginUs = 0;
    uint64_t responderReceiveUs = 0;
    uint64_t responderSendUs = 0;
    uint32_t responderTick = 0;
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.u32("ping_id", m.pingId);
        v.u64("echoed_origin_us", m.echoedOriginUs);
        v.u64("responder_receive_us", m.responderReceiveUs);
        v.u64("responder_send_us", m.responderSendUs);
        v.u32("responder_tick", m.responderTick);
    }
    // The responder can't send before it received.
    const char* invalid() const;
};

struct Reject {
    static constexpr MessageKind kKind = MessageKind::Reject;
    ReasonCode reason = ReasonCode::Normal;
    std::string_view diagnostic;   // no paths, tickets or unchecked remote text
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.reason("reason_code", m.reason);
        v.string("diagnostic", m.diagnostic, 0, kMaxDiagnosticBytes);
    }
    const char* invalid() const { return nullptr; }
};

struct Goodbye {
    static constexpr MessageKind kKind = MessageKind::Goodbye;
    ReasonCode reason = ReasonCode::Normal;
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.reason("reason_code", m.reason);
    }
    const char* invalid() const { return nullptr; }
};

struct ResyncRequest {
    static constexpr MessageKind kKind = MessageKind::ResyncRequest;
    uint32_t lastAppliedServerTick = 0;
    uint8_t reason = 0;
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.u32("last_applied_server_tick", m.lastAppliedServerTick);
        v.u8("reason", m.reason);
    }
    const char* invalid() const { return nullptr; }
};

// One part of a delta-compressed snapshot (Multiplayer/SnapshotDelta.h): the changes in
// slots first_slot..first_slot+slot_count-1 since the baseline snapshot, bit-packed in
// `payload`. Slots no part of the snapshot covers are unchanged. Without kHasBaseline the
// changes are against empty slots (a keyframe). Each part stands alone; the snapshot is
// complete, and may be acknowledged, once all part_count parts have arrived.
struct DeltaSnapshot {
    static constexpr MessageKind kKind = MessageKind::DeltaSnapshot;
    static constexpr uint8_t kHasBaseline = 1;
    uint32_t snapshotTick = 0;
    uint32_t baselineTick = 0;   // 0 without kHasBaseline
    uint32_t requiredControlSerial = 0;
    uint8_t flags = 0;
    uint8_t partIndex = 0;
    uint8_t partCount = 1;
    uint16_t firstSlot = 0;
    uint16_t slotCount = 0;
    uint16_t changedCount = 0;   // slots in the part that changed
    std::span<const uint8_t> payload;
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.u32("snapshot_tick", m.snapshotTick);
        v.u32("baseline_tick", m.baselineTick);
        v.u32("required_control_serial", m.requiredControlSerial);
        v.bits8("flags", m.flags, kHasBaseline);
        v.u8("part_index", m.partIndex, 0, 254);
        v.u8("part_count", m.partCount, 1, 255);
        v.u16("first_slot", m.firstSlot, 0, kMaxDeltaSlots - 1);
        v.u16("slot_count", m.slotCount, 0, kMaxDeltaSlots);
        v.u16("changed_count", m.changedCount, 0, kMaxDeltaSlots);
        v.bytes("payload", m.payload, 0, kMaxUnreliableMessageBytes);
    }
    // part_index < part_count; the slots fit kMaxDeltaSlots; changed_count <= slot_count;
    // a baseline tick only with kHasBaseline, and before snapshot_tick.
    const char* invalid() const;
};

// The client's newest complete snapshot, and which of the 32 before it are also complete
// (bit i: latest_tick - 1 - i), sent every tick so a lost ack costs nothing.
struct SnapshotAck {
    static constexpr MessageKind kKind = MessageKind::SnapshotAck;
    uint32_t latestTick = 0;
    uint32_t receivedBits = 0;
    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.u32("latest_tick", m.latestTick);
        v.u32("received_bits", m.receivedBits);
    }
    const char* invalid() const { return nullptr; }
};

// Alternative i is message kind i + 1.
using MessageBody = std::variant<
    Hello, Challenge, AuthTicket, Welcome, BaselineBegin, Spawn, Despawn, Ownership, InitialState, BaselineEnd, Ready,
    Active, Inputs, Snapshot, Command, GameEvent, Ping, Pong, Reject, Goodbye, ResyncRequest, DeltaSnapshot,
    SnapshotAck>;
static_assert(std::variant_size_v<MessageBody> == kLastMessageKind);

inline MessageKind kindOf(const MessageBody& body) { return MessageKind(body.index() + 1); }

struct DecodedMessage {
    MessageHeader header;
    MessageBody body;
};

struct DecodeOptions {
    bool checkDelivery = true;
    Delivery delivery = Delivery::ReliableOrdered;   // how it actually arrived
    bool checkDirection = true;
    PeerRole receiver = PeerRole::Host;   // who is decoding it
};

// The header alone: magic, version, a known kind, no flags, the payload length matching
// the bytes, and the size limit of the kind's delivery.
WireStatus decodeHeader(std::span<const uint8_t> bytes, MessageHeader& out);
// The whole message. `out` is only meaningful when this succeeds.
WireStatus decodeMessage(std::span<const uint8_t> bytes, const DecodeOptions& options, DecodedMessage& out);
// Header and payload into `out`; `written` is the message's size. Fails without a usable
// result if a field is out of range or `out` is too small.
WireStatus encodeMessage(
    const MessageBody& body, const Nonce& session, uint32_t senderTick, std::span<uint8_t> out, uint32_t& written);

// State records: entity u64, incarnation u32, profile_id u16, state_bytes u16, state.

struct StateRecord {
    uint64_t entity = 0;
    uint32_t incarnation = 0;
    uint16_t profileId = 0;
    std::span<const uint8_t> state;
};

// Appends one record. Fails (and leaves the writer failed) on an out-of-range field.
bool writeStateRecord(WireWriter& writer, const StateRecord& record);

// Walks the records of a decoded InitialState or Snapshot.
class StateRecordReader {
public:
    StateRecordReader(std::span<const uint8_t> records, uint16_t count) : m_reader(records), m_left(count) {}
    bool next(StateRecord& out);

private:
    WireReader m_reader;
    uint16_t m_left;
};

// Built-in state profiles 1 (engine.pose.v1) and 2 (engine.rigidbody.v1) share this
// 53-byte payload.
inline constexpr uint16_t kProfilePose = 1;
inline constexpr uint16_t kProfileRigidBody = 2;

struct PoseState {
    NetVec3 position;
    NetQuat rotation;
    NetVec3 linearVelocity;
    NetVec3 angularVelocity;
    uint8_t flags = 0;
    // kGrounded: a character standing on the ground (what prediction compares).
    static constexpr uint8_t kSleeping = 1, kTeleport = 2, kGrounded = 4;

    template <class Self, class V>
    static void fields(Self& m, V& v) {
        v.vec3("position", m.position, 32768.0f);
        v.quat("rotation", m.rotation);
        v.vec3("linear_velocity", m.linearVelocity, 512.0f);
        v.vec3("angular_velocity", m.angularVelocity, 1024.0f);
        v.bits8("flags", m.flags, kSleeping | kTeleport | kGrounded);
    }
    // The rotation's length must be within 0.99..1.01.
    const char* invalid() const;
    // The rotation scaled to unit length; decoding leaves it as sent.
    NetQuat normalizedRotation() const;
    friend bool operator==(const PoseState&, const PoseState&) = default;
};

WireStatus encodePose(const PoseState& pose, std::span<uint8_t> out, uint32_t& written);
// Exactly 53 bytes, no more.
WireStatus decodePose(std::span<const uint8_t> bytes, PoseState& out);

// The wire format as Markdown, generated from the fields above (docs/NetworkProtocol.md).
std::string protocolReferenceMarkdown();

}   // namespace shockwave::net
