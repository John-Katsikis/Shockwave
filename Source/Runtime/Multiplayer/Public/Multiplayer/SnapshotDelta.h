#pragma once

// Delta-compressed snapshots: the host sends each client only what changed since a
// snapshot that client has acknowledged, bit-packed into DeltaSnapshot messages that each
// fit max_snapshot_bytes (at most 1152 bytes, the Steam Datagram Relay limit, so nothing
// is ever fragmented).
//
// - NetEntityState is one slot's quantized state: 32 bytes of plain data that both sides
//   hold bit for bit identically, so a client rebuilds exactly the host's snapshot.
// - SnapshotHistory (host, one per session) keeps the last 64 ticks of every slot.
// - SnapshotDeltaCompressor (host, one per client) tracks that client's acknowledgements
//   and encodes a tick against the newest acknowledged snapshot still in the history, or
//   as a keyframe against empty slots when there is none.
// - SnapshotDeltaDecoder (client) applies parts straight from the received bytes into its
//   own 64-tick ring and acknowledges complete snapshots with SnapshotAck.
//
// Slots are numbered 0..slotCount-1; which entity is in which slot is settled by reliable
// lifecycle messages, never by a snapshot. A slot is empty while its incarnation is 0.
// Everything is allocated in the constructors: storing, encoding, acknowledging and
// applying never allocate. The bit layout is in docs/Networking.md (Delta snapshots).

#include <Network/Messages.h>

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace shockwave::net {

// Quantization steps: 1/1024 m, 1/64 m/s, 1/32 rad/s, and rotations as the smallest three
// components at 10 bits each (about 0.1 degree).
inline constexpr float kNetPositionScale = 1024.0f;
inline constexpr float kNetVelocityScale = 64.0f;
inline constexpr float kNetAngularScale = 32.0f;
inline constexpr int32_t kNetMaxPosition = 32768 * 1024;   // engine.pose.v1's 32768 m

struct alignas(16) NetEntityState {
    std::array<int32_t, 3> position{};   // 1/1024 m
    uint32_t rotation = 0;   // bits 30-31: the largest component, dropped; then 3 x 10-bit signed
    std::array<int16_t, 3> velocity{};   // 1/64 m/s
    std::array<int16_t, 3> angularVelocity{};   // 1/32 rad/s
    uint8_t flags = 0;   // PoseState::kSleeping, kTeleport
    uint8_t reserved = 0;   // always 0
    uint16_t incarnation = 0;   // low 16 bits of the entity's incarnation; 0: the slot is empty

    bool active() const { return incarnation != 0; }
    friend bool operator==(const NetEntityState&, const NetEntityState&) = default;
};
static_assert(sizeof(NetEntityState) == 32 && alignof(NetEntityState) == 16);
static_assert(std::is_trivially_copyable_v<NetEntityState> && std::is_standard_layout_v<NetEntityState>);

// A validated pose (PoseState::invalid() is null) to its quantized state. Velocities past
// 511.98 m/s and angular velocities past 1023.97 rad/s become the largest step; the
// rotation is normalized first. `incarnation` must not be 0 modulo 65536.
NetEntityState quantizeState(const PoseState& pose, uint32_t incarnation);
// Back to a pose, with a unit-length rotation.
PoseState dequantizeState(const NetEntityState& state);
// Null if the state could have come from quantizeState (or is an empty slot, all zero),
// otherwise the first field that couldn't.
const char* invalidState(const NetEntityState& state);

// Wire and payload limits of one delta part.
inline constexpr uint32_t kDeltaSnapshotOverheadBytes = kHeaderBytes + 23;   // header, fields, payload count
inline constexpr uint32_t kMaxDeltaEntityBits = 283;   // one changed slot, worst case
inline constexpr uint16_t kDeltaHistoryTicks = 64;

// The host's last kDeltaHistoryTicks snapshots of every slot.
class SnapshotHistory {
public:
    explicit SnapshotHistory(uint16_t slotCount);   // 1..kMaxDeltaSlots
    SnapshotHistory(const SnapshotHistory&) = delete;
    SnapshotHistory& operator=(const SnapshotHistory&) = delete;

    uint16_t slotCount() const { return m_slotCount; }
    // Copies one tick's slots (exactly slotCount of them). The tick must be newer than the
    // last one stored and every state valid; false stores nothing.
    bool store(uint32_t tick, std::span<const NetEntityState> slots);
    // The slots of a stored tick, or null once it has left the history.
    const NetEntityState* find(uint32_t tick) const;
    bool hasLatest() const { return m_stored > 0; }
    uint32_t latestTick() const { return m_latest; }

private:
    uint16_t m_slotCount;
    std::unique_ptr<NetEntityState[]> m_slots;   // kDeltaHistoryTicks x slotCount
    std::array<uint32_t, kDeltaHistoryTicks> m_ticks{};
    std::array<bool, kDeltaHistoryTicks> m_used{};
    uint32_t m_latest = 0;
    uint64_t m_stored = 0;
};

// One encoded part: a whole wire message (header included).
struct DeltaPart {
    std::span<const uint8_t> message;
    uint16_t changed = 0;
};

struct DeltaEncodeStats {
    uint32_t parts = 0;
    uint32_t bytes = 0;   // all parts, headers included
    uint32_t changed = 0;
    uint32_t payloadBits = 0;   // all parts, padding excluded
    bool keyframe = false;
};

// The host's delta encoder, shared by every client of a session. Clients whose newest
// acknowledged snapshot is the same tick get the same parts, so a tick costs one encode
// per distinct baseline, not one per client. Each tick's encodings are kept until a newer
// tick (or another control serial or session) is encoded; with the default cacheSize every
// baseline the history can hold fits, so parts handed out for a tick stay valid all tick.
class SnapshotDeltaEncoder {
public:
    // maxMessageBytes: the session's max_snapshot_bytes, 512..1152. cacheSize: 1..65.
    SnapshotDeltaEncoder(
        const SnapshotHistory& history, uint32_t maxMessageBytes = kMaxUnreliableMessageBytes,
        uint16_t cacheSize = kDeltaHistoryTicks + 1);
    SnapshotDeltaEncoder(const SnapshotDeltaEncoder&) = delete;
    SnapshotDeltaEncoder& operator=(const SnapshotDeltaEncoder&) = delete;

    const SnapshotHistory& history() const { return m_history; }
    uint32_t payloadBitsPerPart() const { return m_payloadBits; }

    // The stored snapshot for `tick` against `baseline` (null: a keyframe), as parts that
    // each fit maxMessageBytes. False if either tick isn't stored.
    bool encode(
        uint32_t tick, const uint32_t* baseline, uint32_t requiredControlSerial, const Nonce& session,
        std::span<const DeltaPart>& parts, const DeltaEncodeStats** stats = nullptr);
    // Encodes done since construction (cache misses), for measurements.
    uint64_t encodeCount() const { return m_encodes; }

private:
    struct PartInfo {
        uint16_t first = 0, count = 0, changed = 0;
        uint32_t bits = 0;
    };
    struct Entry {
        bool used = false, keyframe = false;
        uint32_t baseline = 0;
        uint32_t partCount = 0;
        DeltaEncodeStats stats;
    };
    bool encodeInto(
        Entry& entry, DeltaPart* parts, uint8_t* messages, uint32_t tick, const NetEntityState* cur,
        const NetEntityState* base);

    const SnapshotHistory& m_history;
    uint32_t m_maxMessageBytes;
    uint32_t m_payloadBits;
    uint32_t m_maxParts;
    uint16_t m_cacheSize;
    std::unique_ptr<uint8_t[]> m_payloads;   // m_maxParts x payload bytes, reused by every encode
    std::unique_ptr<PartInfo[]> m_info;
    std::unique_ptr<uint8_t[]> m_messages;   // cacheSize x m_maxParts x maxMessageBytes
    std::unique_ptr<DeltaPart[]> m_parts;   // cacheSize x m_maxParts
    std::unique_ptr<Entry[]> m_entries;
    bool m_hasKey = false;
    uint32_t m_tick = 0, m_serial = 0;
    Nonce m_session{};
    uint16_t m_next = 0;   // the entry to reuse when all are taken
    uint64_t m_encodes = 0;
};

// One client's view of the encoder: which snapshot it has acknowledged.
class SnapshotDeltaCompressor {
public:
    // Shares a session's encoder.
    explicit SnapshotDeltaCompressor(SnapshotDeltaEncoder& encoder);
    // With an encoder of its own (one cached encoding).
    SnapshotDeltaCompressor(const SnapshotHistory& history, uint32_t maxMessageBytes = kMaxUnreliableMessageBytes);
    SnapshotDeltaCompressor(const SnapshotDeltaCompressor&) = delete;
    SnapshotDeltaCompressor& operator=(const SnapshotDeltaCompressor&) = delete;

    // An acknowledgement from this client. Acks arrive unreliably and in any order: the
    // newest one for a tick this compressor has sent wins; others are ignored.
    void acknowledge(const SnapshotAck& ack);
    // Forgets every acknowledgement, so the next snapshot is a keyframe (after a resync).
    void reset();
    bool hasBaseline() const { return m_acked; }
    uint32_t baselineTick() const { return m_ackedTick; }

    // Encodes the stored snapshot for `tick` against the newest acknowledged one still in
    // the history (a keyframe without one). False if the tick isn't stored. The parts stay
    // valid as the encoder's encodings do.
    bool encode(uint32_t tick, uint32_t requiredControlSerial, const Nonce& session, std::span<const DeltaPart>& parts);
    const DeltaEncodeStats& lastStats() const { return m_stats; }
    uint32_t payloadBitsPerPart() const { return m_encoder->payloadBitsPerPart(); }

private:
    std::unique_ptr<SnapshotDeltaEncoder> m_own;
    SnapshotDeltaEncoder* m_encoder;
    bool m_acked = false;
    uint32_t m_ackedTick = 0;
    bool m_sent = false;
    uint32_t m_lastSent = 0;
    DeltaEncodeStats m_stats;
};

enum class DeltaApplyResult : uint8_t {
    Applied,   // the part is in; the snapshot still waits for other parts
    Completed,   // the part completed its snapshot
    Duplicate,   // already have that part
    Stale,   // too old for the ring
    MissingBaseline,   // its baseline isn't a complete snapshot here
    Malformed,   // nothing was applied
};
const char* deltaApplyResultName(DeltaApplyResult result);

// The client side: rebuilds snapshots from parts and says which are complete.
class SnapshotDeltaDecoder {
public:
    explicit SnapshotDeltaDecoder(uint16_t slotCount);
    SnapshotDeltaDecoder(const SnapshotDeltaDecoder&) = delete;
    SnapshotDeltaDecoder& operator=(const SnapshotDeltaDecoder&) = delete;

    uint16_t slotCount() const { return m_slotCount; }
    // Decodes a part straight from its payload bytes (no copy) into the snapshot for its
    // tick. A part that is malformed in any way changes nothing.
    DeltaApplyResult apply(const DeltaSnapshot& part);
    // The slots of a complete snapshot, or null.
    const NetEntityState* complete(uint32_t tick) const;
    bool hasComplete() const { return m_hasComplete; }
    uint32_t latestCompleteTick() const { return m_latestComplete; }
    // What to send back this tick.
    SnapshotAck ack() const;
    // Drops every snapshot (after a resync or a new session).
    void reset();

private:
    struct Entry {
        uint32_t tick = 0;
        uint32_t baselineTick = 0;
        bool used = false;
        bool completeSnapshot = false;
        bool hasBaseline = false;
        uint8_t partCount = 0;
        uint16_t partsReceived = 0;
        std::bitset<256> received;
        std::array<uint32_t, 255> ranges{};   // first slot << 16 | slot count, per received part
    };
    uint16_t m_slotCount;
    std::unique_ptr<NetEntityState[]> m_slots;   // kDeltaHistoryTicks x slotCount
    std::unique_ptr<Entry[]> m_entries;
    std::unique_ptr<uint16_t[]> m_stagedSlots;
    std::unique_ptr<NetEntityState[]> m_stagedStates;
    bool m_hasComplete = false;
    uint32_t m_latestComplete = 0;
    bool m_seen = false;
    uint32_t m_newestSeen = 0;
};

}   // namespace shockwave::net
