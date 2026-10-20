// Delta snapshots (Multiplayer/SnapshotDelta.h). A part's payload is, for each slot from
// first_slot on, one "changed" bit and, if set, the slot's changes:
//
//   fresh      1 bit: against an empty slot rather than the baseline's
//   fields     6 bits: position, rotation, velocity, angular velocity, flags, incarnation
//   position   per component: 5-bit width n, then the zigzag difference's low n-1 bits
//   rotation   1 bit "relative": 3 x (4-bit width, low n-1 bits); otherwise 32 raw bits
//   velocity, angular velocity   per component: 5-bit width, low n-1 bits
//   flags      8 bits; incarnation 16 bits
//
// Bits fill each byte from its least significant end; the last byte is padded with zeros.
// A fresh slot with no fields set has been emptied.
#include <Multiplayer/SnapshotDelta.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace shockwave::net {

namespace {

constexpr uint8_t kFieldPosition = 1, kFieldRotation = 2, kFieldVelocity = 4, kFieldAngular = 8, kFieldFlags = 16,
                  kFieldIncarnation = 32;
constexpr uint8_t kFlagMask = PoseState::kSleeping | PoseState::kTeleport | PoseState::kGrounded;
constexpr float kRotationScale = 511.0f * 1.41421356237f;   // +-1/sqrt(2) to +-511
const NetEntityState kEmpty{};

// ---- Bits -------------------------------------------------------------------------------

// Writes need 8 bytes of slack after the buffer's last byte: bits go in with one 64-bit load
// and store (little endian), which leave the bits around them as they were.
inline constexpr uint32_t kBitWriterSlackBytes = 8;
static_assert(std::endian::native == std::endian::little);

class BitWriter {
public:
    BitWriter(uint8_t* data, uint32_t capacityBits) : m_data(data), m_capacity(capacityBits) {}
    // The low `count` bits of `value` (count <= 64).
    void bits(uint64_t value, uint32_t count) {
        if (count > m_capacity - m_used) {
            m_overflow = true;
            return;
        }
        if (count > 56) {
            put(value & 0xFFFFFFFFu, 32);
            value >>= 32;
            count -= 32;
        }
        put(value, count);
    }
    void bit(bool value) { bits(value ? 1 : 0, 1); }
    // Appends another writer's bits.
    void append(const uint8_t* data, uint32_t count) {
        for (uint32_t i = 0; i < count; i += 32) {
            uint32_t word;
            std::memcpy(&word, data + (i >> 3), 4);   // the source has slack too
            bits(word, std::min<uint32_t>(32, count - i));
        }
    }
    uint32_t used() const { return m_used; }
    bool overflow() const { return m_overflow; }

private:
    void put(uint64_t value, uint32_t count) {   // count <= 56
        if (!count) return;
        const uint32_t offset = m_used & 7;
        const uint64_t mask = ((uint64_t(1) << count) - 1) << offset;
        uint64_t word;
        std::memcpy(&word, m_data + (m_used >> 3), 8);
        word = (word & ~mask) | ((value << offset) & mask);
        std::memcpy(m_data + (m_used >> 3), &word, 8);
        m_used += count;
    }

    uint8_t* m_data;
    uint32_t m_capacity;
    uint32_t m_used = 0;
    bool m_overflow = false;
};

class BitReader {
public:
    explicit BitReader(std::span<const uint8_t> data) : m_data(data), m_size(uint32_t(data.size()) * 8) {}
    bool bits(uint32_t count, uint64_t& out) {
        if (count > m_size - m_used) return false;
        out = 0;
        for (uint32_t done = 0; done < count;) {
            const uint32_t offset = m_used & 7;
            const uint32_t take = std::min(8 - offset, count - done);
            out |= uint64_t((m_data[m_used >> 3] >> offset) & ((1u << take) - 1)) << done;
            done += take;
            m_used += take;
        }
        return true;
    }
    bool bit(bool& out) {
        uint64_t v = 0;
        if (!bits(1, v)) return false;
        out = v != 0;
        return true;
    }
    // True if what's left is under a byte and all zero.
    bool paddedEnd() const {
        if (m_size - m_used >= 8) return false;
        for (uint32_t i = m_used; i < m_size; ++i)
            if ((m_data[i >> 3] >> (i & 7)) & 1) return false;
        return true;
    }

private:
    std::span<const uint8_t> m_data;
    uint32_t m_size;
    uint32_t m_used = 0;
};

uint64_t zigzag(int64_t v) { return (uint64_t(v) << 1) ^ uint64_t(v >> 63); }
int64_t unzigzag(uint64_t z) { return int64_t(z >> 1) ^ -int64_t(z & 1); }

// A difference as a width (of `widthBits` bits) and the value's bits below its top one.
uint32_t valueCost(int64_t difference, uint32_t widthBits) {
    const uint32_t n = uint32_t(std::bit_width(zigzag(difference)));
    return widthBits + (n ? n - 1 : 0);
}
void writeValue(BitWriter& w, int64_t difference, uint32_t widthBits) {
    const uint64_t z = zigzag(difference);
    const uint32_t n = uint32_t(std::bit_width(z));
    w.bits(n, widthBits);
    if (n > 1) w.bits(z, n - 1);
}
bool readValue(BitReader& r, uint32_t widthBits, uint32_t maxWidth, int64_t& out) {
    uint64_t n = 0;
    if (!r.bits(widthBits, n) || n > maxWidth) return false;
    if (n == 0) {
        out = 0;
        return true;
    }
    uint64_t low = 0;
    if (!r.bits(uint32_t(n - 1), low)) return false;
    out = unzigzag((uint64_t(1) << (n - 1)) | low);
    return true;
}

// Widths: position differences are at most 2^26 (zigzag 2^27, 28 bits), 16-bit ones
// under 2^17, rotation components under 2^11.
constexpr uint32_t kPositionWidthBits = 5, kPositionMaxWidth = 28;
constexpr uint32_t kShortWidthBits = 5, kShortMaxWidth = 17;
constexpr uint32_t kRotationWidthBits = 4, kRotationMaxWidth = 11;

int32_t rotationIndex(uint32_t r) { return int32_t(r >> 30); }
int32_t rotationComponent(uint32_t r, int i) {
    const uint32_t raw = (r >> (10 * i)) & 0x3FF;
    return raw & 0x200 ? int32_t(raw) - 0x400 : int32_t(raw);
}

uint32_t relativeRotationBits(uint32_t cur, uint32_t base) {
    uint32_t bits = 1;
    for (int i = 0; i < 3; ++i)
        bits += valueCost(rotationComponent(cur, i) - rotationComponent(base, i), kRotationWidthBits);
    return bits;
}

uint8_t changedFields(const NetEntityState& cur, const NetEntityState& base) {
    uint8_t f = 0;
    if (cur.position != base.position) f |= kFieldPosition;
    if (cur.rotation != base.rotation) f |= kFieldRotation;
    if (cur.velocity != base.velocity) f |= kFieldVelocity;
    if (cur.angularVelocity != base.angularVelocity) f |= kFieldAngular;
    if (cur.flags != base.flags) f |= kFieldFlags;
    if (cur.incarnation != base.incarnation) f |= kFieldIncarnation;
    return f;
}

// One changed slot. The base is the baseline's slot, or an empty one when `fresh`.
void writeEntity(BitWriter& w, const NetEntityState& cur, const NetEntityState& base, bool fresh) {
    const uint8_t fields = changedFields(cur, base);
    w.bit(fresh);
    w.bits(fields, 6);
    if (fields & kFieldPosition)
        for (int i = 0; i < 3; ++i) writeValue(w, int64_t(cur.position[i]) - base.position[i], kPositionWidthBits);
    if (fields & kFieldRotation) {
        const bool relative = rotationIndex(cur.rotation) == rotationIndex(base.rotation) &&
                              relativeRotationBits(cur.rotation, base.rotation) < 33;
        w.bit(relative);
        if (relative)
            for (int i = 0; i < 3; ++i)
                writeValue(
                    w, rotationComponent(cur.rotation, i) - rotationComponent(base.rotation, i), kRotationWidthBits);
        else
            w.bits(cur.rotation, 32);
    }
    if (fields & kFieldVelocity)
        for (int i = 0; i < 3; ++i) writeValue(w, int64_t(cur.velocity[i]) - base.velocity[i], kShortWidthBits);
    if (fields & kFieldAngular)
        for (int i = 0; i < 3; ++i)
            writeValue(w, int64_t(cur.angularVelocity[i]) - base.angularVelocity[i], kShortWidthBits);
    if (fields & kFieldFlags) w.bits(cur.flags, 8);
    if (fields & kFieldIncarnation) w.bits(cur.incarnation, 16);
}

bool readEntity(BitReader& r, const NetEntityState& baseline, NetEntityState& out) {
    bool fresh = false;
    uint64_t fields = 0;
    if (!r.bit(fresh) || !r.bits(6, fields)) return false;
    const NetEntityState& base = fresh ? kEmpty : baseline;
    // A delta needs a live baseline of the same incarnation; nothing else changes it.
    if (!fresh && (!base.active() || (fields & kFieldIncarnation) || fields == 0)) return false;
    out = base;
    int64_t v = 0;
    if (fields & kFieldPosition)
        for (int i = 0; i < 3; ++i) {
            if (!readValue(r, kPositionWidthBits, kPositionMaxWidth, v)) return false;
            v += base.position[i];
            if (v < -kNetMaxPosition || v > kNetMaxPosition) return false;
            out.position[i] = int32_t(v);
        }
    if (fields & kFieldRotation) {
        bool relative = false;
        if (!r.bit(relative)) return false;
        if (relative) {
            uint32_t rot = base.rotation & 0xC0000000u;
            for (int i = 0; i < 3; ++i) {
                if (!readValue(r, kRotationWidthBits, kRotationMaxWidth, v)) return false;
                v += rotationComponent(base.rotation, i);
                if (v < -511 || v > 511) return false;
                rot |= (uint32_t(v) & 0x3FF) << (10 * i);
            }
            out.rotation = rot;
        } else {
            uint64_t raw = 0;
            if (!r.bits(32, raw)) return false;
            out.rotation = uint32_t(raw);
        }
    }
    if (fields & kFieldVelocity)
        for (int i = 0; i < 3; ++i) {
            if (!readValue(r, kShortWidthBits, kShortMaxWidth, v)) return false;
            v += base.velocity[i];
            if (v < -32767 || v > 32767) return false;
            out.velocity[i] = int16_t(v);
        }
    if (fields & kFieldAngular)
        for (int i = 0; i < 3; ++i) {
            if (!readValue(r, kShortWidthBits, kShortMaxWidth, v)) return false;
            v += base.angularVelocity[i];
            if (v < -32767 || v > 32767) return false;
            out.angularVelocity[i] = int16_t(v);
        }
    uint64_t raw = 0;
    if (fields & kFieldFlags) {
        if (!r.bits(8, raw)) return false;
        out.flags = uint8_t(raw);
    }
    if (fields & kFieldIncarnation) {
        if (!r.bits(16, raw)) return false;
        out.incarnation = uint16_t(raw);
    }
    // The fields bits must be exactly what changed, and the result a state the host could have.
    return changedFields(out, base) == fields && !invalidState(out);
}

int16_t quantizeShort(float value, float scale) {
    return int16_t(std::clamp<long>(std::lround(double(value) * scale), -32767, 32767));
}

}   // namespace

// ---- Quantization -----------------------------------------------------------------------

NetEntityState quantizeState(const PoseState& pose, uint32_t incarnation) {
    NetEntityState s;
    const NetVec3& p = pose.position;
    s.position = {
        int32_t(std::clamp<long>(std::lround(double(p.x) * kNetPositionScale), -kNetMaxPosition, kNetMaxPosition)),
        int32_t(std::clamp<long>(std::lround(double(p.y) * kNetPositionScale), -kNetMaxPosition, kNetMaxPosition)),
        int32_t(std::clamp<long>(std::lround(double(p.z) * kNetPositionScale), -kNetMaxPosition, kNetMaxPosition))};
    const NetQuat q = pose.normalizedRotation();
    std::array<float, 4> c = {q.x, q.y, q.z, q.w};
    int largest = 0;
    for (int i = 1; i < 4; ++i)
        if (std::fabs(c[i]) > std::fabs(c[largest])) largest = i;
    const float sign = c[largest] < 0 ? -1.0f : 1.0f;
    uint32_t rot = uint32_t(largest) << 30;
    for (int i = 0, out = 0; i < 4; ++i) {
        if (i == largest) continue;
        const long v = std::clamp<long>(std::lround(double(c[i] * sign) * kRotationScale), -511, 511);
        rot |= (uint32_t(v) & 0x3FF) << (10 * out++);
    }
    s.rotation = rot;
    s.velocity = {
        quantizeShort(pose.linearVelocity.x, kNetVelocityScale),
        quantizeShort(pose.linearVelocity.y, kNetVelocityScale),
        quantizeShort(pose.linearVelocity.z, kNetVelocityScale)};
    s.angularVelocity = {
        quantizeShort(pose.angularVelocity.x, kNetAngularScale),
        quantizeShort(pose.angularVelocity.y, kNetAngularScale),
        quantizeShort(pose.angularVelocity.z, kNetAngularScale)};
    s.flags = pose.flags & kFlagMask;
    s.incarnation = uint16_t(incarnation);
    return s;
}

PoseState dequantizeState(const NetEntityState& s) {
    PoseState pose;
    pose.position = {
        float(s.position[0] / double(kNetPositionScale)), float(s.position[1] / double(kNetPositionScale)),
        float(s.position[2] / double(kNetPositionScale))};
    std::array<float, 4> c{};
    const int largest = rotationIndex(s.rotation);
    float sum = 0;
    for (int i = 0, in = 0; i < 4; ++i) {
        if (i == largest) continue;
        c[i] = float(rotationComponent(s.rotation, in++)) / kRotationScale;
        sum += c[i] * c[i];
    }
    c[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
    const float length = std::sqrt(sum + c[largest] * c[largest]);
    pose.rotation = {c[0] / length, c[1] / length, c[2] / length, c[3] / length};
    pose.linearVelocity = {
        s.velocity[0] / kNetVelocityScale, s.velocity[1] / kNetVelocityScale, s.velocity[2] / kNetVelocityScale};
    pose.angularVelocity = {
        s.angularVelocity[0] / kNetAngularScale, s.angularVelocity[1] / kNetAngularScale,
        s.angularVelocity[2] / kNetAngularScale};
    pose.flags = s.flags;
    return pose;
}

const char* invalidState(const NetEntityState& s) {
    if (!s.active()) return s == kEmpty ? nullptr : "incarnation";
    for (int32_t p : s.position)
        if (p < -kNetMaxPosition || p > kNetMaxPosition) return "position";
    for (int i = 0; i < 3; ++i)
        if (rotationComponent(s.rotation, i) < -511) return "rotation";
    for (int16_t v : s.velocity)
        if (v == -32768) return "velocity";
    for (int16_t v : s.angularVelocity)
        if (v == -32768) return "angular_velocity";
    if (s.flags & ~kFlagMask) return "flags";
    if (s.reserved) return "reserved";
    return nullptr;
}

// ---- History ----------------------------------------------------------------------------

SnapshotHistory::SnapshotHistory(uint16_t slotCount)
    : m_slotCount(std::clamp<uint16_t>(slotCount, 1, kMaxDeltaSlots)),
      m_slots(std::make_unique<NetEntityState[]>(size_t(kDeltaHistoryTicks) * m_slotCount)) {}

bool SnapshotHistory::store(uint32_t tick, std::span<const NetEntityState> slots) {
    if (slots.size() != m_slotCount) return false;
    if (m_stored > 0 && !tickNewer(tick, m_latest)) return false;
    for (const NetEntityState& s : slots)
        if (invalidState(s)) return false;
    const size_t index = tick % kDeltaHistoryTicks;
    std::memcpy(&m_slots[index * m_slotCount], slots.data(), slots.size_bytes());
    // Ticks skipped since the last one leave the history: their entries hold older ticks.
    m_ticks[index] = tick;
    m_used[index] = true;
    m_latest = tick;
    ++m_stored;
    return true;
}

const NetEntityState* SnapshotHistory::find(uint32_t tick) const {
    if (m_stored == 0 || tickNewer(tick, m_latest) || m_latest - tick >= kDeltaHistoryTicks) return nullptr;
    const size_t index = tick % kDeltaHistoryTicks;
    if (!m_used[index] || m_ticks[index] != tick) return nullptr;
    return &m_slots[index * m_slotCount];
}

// ---- Compressor -------------------------------------------------------------------------

SnapshotDeltaEncoder::SnapshotDeltaEncoder(const SnapshotHistory& history, uint32_t maxMessageBytes, uint16_t cacheSize)
    : m_history(history), m_maxMessageBytes(std::clamp<uint32_t>(maxMessageBytes, 512, kMaxUnreliableMessageBytes)),
      m_payloadBits((m_maxMessageBytes - kDeltaSnapshotOverheadBytes) * 8),
      m_cacheSize(std::clamp<uint16_t>(cacheSize, 1, kDeltaHistoryTicks + 1)) {
    // Worst case: every slot changed and as large as can be, a part ending early whenever
    // the next slot doesn't fit.
    const uint32_t perSlot = 1 + kMaxDeltaEntityBits;
    m_maxParts =
        (uint32_t(history.slotCount()) * perSlot + (m_payloadBits - perSlot) - 1) / (m_payloadBits - perSlot) + 1;
    m_payloads = std::make_unique<uint8_t[]>(size_t(m_maxParts) * (m_payloadBits / 8) + kBitWriterSlackBytes);
    m_info = std::make_unique<PartInfo[]>(m_maxParts);
    m_messages = std::make_unique<uint8_t[]>(size_t(m_cacheSize) * m_maxParts * m_maxMessageBytes);
    m_parts = std::make_unique<DeltaPart[]>(size_t(m_cacheSize) * m_maxParts);
    m_entries = std::make_unique<Entry[]>(m_cacheSize);
}

bool SnapshotDeltaEncoder::encode(
    uint32_t tick, const uint32_t* baseline, uint32_t requiredControlSerial, const Nonce& session,
    std::span<const DeltaPart>& parts, const DeltaEncodeStats** stats) {
    parts = {};
    const NetEntityState* cur = m_history.find(tick);
    if (!cur) return false;
    const NetEntityState* base = baseline ? m_history.find(*baseline) : nullptr;
    if (baseline && !base) return false;
    // A new tick (or serial, or session) starts the cache over.
    if (!m_hasKey || tick != m_tick || requiredControlSerial != m_serial || session != m_session) {
        for (uint16_t i = 0; i < m_cacheSize; ++i) m_entries[i].used = false;
        m_hasKey = true;
        m_tick = tick;
        m_serial = requiredControlSerial;
        m_session = session;
        m_next = 0;
    }
    uint16_t slot = m_cacheSize;
    for (uint16_t i = 0; i < m_cacheSize && slot == m_cacheSize; ++i) {
        const Entry& e = m_entries[i];
        if (e.used && e.keyframe == !baseline && (!baseline || e.baseline == *baseline)) slot = i;
    }
    if (slot == m_cacheSize) {
        for (uint16_t i = 0; i < m_cacheSize && slot == m_cacheSize; ++i)
            if (!m_entries[i].used) slot = i;
        if (slot == m_cacheSize) {   // full: reuse the entries in turn
            slot = m_next;
            m_next = uint16_t((m_next + 1) % m_cacheSize);
        }
        Entry& e = m_entries[slot];
        e.used = false;
        e.keyframe = !baseline;
        e.baseline = baseline ? *baseline : 0;
        if (!encodeInto(
                e, &m_parts[size_t(slot) * m_maxParts], &m_messages[size_t(slot) * m_maxParts * m_maxMessageBytes],
                tick, cur, base))
            return false;
        e.used = true;
        ++m_encodes;
    }
    const Entry& e = m_entries[slot];
    parts = std::span<const DeltaPart>(&m_parts[size_t(slot) * m_maxParts], e.partCount);
    if (stats) *stats = &e.stats;
    return true;
}

bool SnapshotDeltaEncoder::encodeInto(
    Entry& entry, DeltaPart* outParts, uint8_t* messages, uint32_t tick, const NetEntityState* cur,
    const NetEntityState* base) {
    const uint16_t slots = m_history.slotCount();
    const uint32_t payloadBytes = m_payloadBits / 8;

    uint32_t partCount = 0;
    PartInfo* open = nullptr;
    BitWriter writer(nullptr, 0);
    std::array<uint8_t, (kMaxDeltaEntityBits + 7) / 8 + kBitWriterSlackBytes> entityBytes{};
    for (uint16_t s = 0; s < slots; ++s) {
        const NetEntityState& c = cur[s];
        const NetEntityState& b = base ? base[s] : kEmpty;
        if (c == b) continue;
        // Against the baseline when it holds the same live entity, else from an empty slot.
        const bool fresh = !base || !b.active() || b.incarnation != c.incarnation;
        BitWriter entity(entityBytes.data(), kMaxDeltaEntityBits);
        writeEntity(entity, c, fresh ? kEmpty : b, fresh);
        if (entity.overflow()) return false;   // can't happen: kMaxDeltaEntityBits is the most
        // Every slot from the part's first to this one costs a "changed" bit.
        uint32_t gap = open ? uint32_t(s - (open->first + open->count)) : 0;
        if (open && open->bits + gap + 1 + entity.used() > m_payloadBits) open = nullptr;
        if (!open) {
            gap = 0;
            if (partCount == m_maxParts) return false;   // can't happen; see the constructor
            open = &m_info[partCount++];
            *open = {s, 0, 0, 0};
            writer = BitWriter(&m_payloads[size_t(partCount - 1) * payloadBytes], m_payloadBits);
        }
        for (uint32_t left = gap; left;) {   // unchanged slots
            const uint32_t n = std::min<uint32_t>(left, 32);
            writer.bits(0, n);
            left -= n;
        }
        writer.bit(true);
        writer.append(entityBytes.data(), entity.used());
        open->bits = writer.used();
        open->count = uint16_t(s - open->first + 1);
        ++open->changed;
    }
    if (partCount == 0) m_info[partCount++] = {};   // nothing changed: one empty part

    entry.stats = {partCount, 0, 0, 0, base == nullptr};
    for (uint32_t i = 0; i < partCount; ++i) {
        const PartInfo& info = m_info[i];
        uint8_t* message = &messages[size_t(i) * m_maxMessageBytes];
        uint8_t* payload = &m_payloads[size_t(i) * payloadBytes];
        if (info.bits % 8) payload[info.bits / 8] &= uint8_t((1u << (info.bits % 8)) - 1);   // zero padding
        DeltaSnapshot part;
        part.snapshotTick = tick;
        part.baselineTick = entry.baseline;
        part.requiredControlSerial = m_serial;
        part.flags = base ? DeltaSnapshot::kHasBaseline : 0;
        part.partIndex = uint8_t(i);
        part.partCount = uint8_t(partCount);
        part.firstSlot = info.first;
        part.slotCount = info.count;
        part.changedCount = info.changed;
        part.payload = std::span<const uint8_t>(payload, (info.bits + 7) / 8);
        uint32_t written = 0;
        const WireStatus status =
            encodeMessage(MessageBody(part), m_session, tick, std::span<uint8_t>(message, m_maxMessageBytes), written);
        if (!status) return false;
        outParts[i] = {std::span<const uint8_t>(message, written), info.changed};
        entry.stats.bytes += written;
        entry.stats.changed += info.changed;
        entry.stats.payloadBits += info.bits;
    }
    entry.partCount = partCount;
    return true;
}

SnapshotDeltaCompressor::SnapshotDeltaCompressor(SnapshotDeltaEncoder& encoder) : m_encoder(&encoder) {}

SnapshotDeltaCompressor::SnapshotDeltaCompressor(const SnapshotHistory& history, uint32_t maxMessageBytes)
    : m_own(std::make_unique<SnapshotDeltaEncoder>(history, maxMessageBytes, 1)), m_encoder(m_own.get()) {}

void SnapshotDeltaCompressor::acknowledge(const SnapshotAck& ack) {
    if (!m_sent || tickNewer(ack.latestTick, m_lastSent)) return;   // never sent
    if (m_acked && !tickNewer(ack.latestTick, m_ackedTick)) return;   // older news
    m_acked = true;
    m_ackedTick = ack.latestTick;
}

void SnapshotDeltaCompressor::reset() { m_acked = false; }

bool SnapshotDeltaCompressor::encode(
    uint32_t tick, uint32_t requiredControlSerial, const Nonce& session, std::span<const DeltaPart>& parts) {
    parts = {};
    const SnapshotHistory& history = m_encoder->history();
    if (!history.find(tick)) return false;
    const bool useBaseline = m_acked && tickNewer(tick, m_ackedTick) && history.find(m_ackedTick);
    const DeltaEncodeStats* stats = nullptr;
    if (!m_encoder->encode(tick, useBaseline ? &m_ackedTick : nullptr, requiredControlSerial, session, parts, &stats))
        return false;
    m_stats = *stats;
    m_sent = true;
    m_lastSent = tick;
    return true;
}

// ---- Decoder ----------------------------------------------------------------------------

const char* deltaApplyResultName(DeltaApplyResult result) {
    switch (result) {
    case DeltaApplyResult::Applied: return "applied";
    case DeltaApplyResult::Completed: return "completed";
    case DeltaApplyResult::Duplicate: return "duplicate";
    case DeltaApplyResult::Stale: return "stale";
    case DeltaApplyResult::MissingBaseline: return "missing baseline";
    case DeltaApplyResult::Malformed: return "malformed";
    }
    return "unknown";
}

SnapshotDeltaDecoder::SnapshotDeltaDecoder(uint16_t slotCount)
    : m_slotCount(std::clamp<uint16_t>(slotCount, 1, kMaxDeltaSlots)),
      m_slots(std::make_unique<NetEntityState[]>(size_t(kDeltaHistoryTicks) * m_slotCount)),
      m_entries(std::make_unique<Entry[]>(kDeltaHistoryTicks)),
      m_stagedSlots(std::make_unique<uint16_t[]>(m_slotCount)),
      m_stagedStates(std::make_unique<NetEntityState[]>(m_slotCount)) {}

void SnapshotDeltaDecoder::reset() {
    for (size_t i = 0; i < kDeltaHistoryTicks; ++i) m_entries[i].used = false;
    m_hasComplete = m_seen = false;
}

const NetEntityState* SnapshotDeltaDecoder::complete(uint32_t tick) const {
    const Entry& e = m_entries[tick % kDeltaHistoryTicks];
    if (!e.used || !e.completeSnapshot || e.tick != tick) return nullptr;
    return &m_slots[size_t(tick % kDeltaHistoryTicks) * m_slotCount];
}

SnapshotAck SnapshotDeltaDecoder::ack() const {
    SnapshotAck a;
    if (!m_hasComplete) return a;
    a.latestTick = m_latestComplete;
    for (uint32_t i = 0; i < 32; ++i)
        if (complete(m_latestComplete - 1 - i)) a.receivedBits |= 1u << i;
    return a;
}

DeltaApplyResult SnapshotDeltaDecoder::apply(const DeltaSnapshot& part) {
    if (part.invalid() || uint32_t(part.firstSlot) + part.slotCount > m_slotCount) return DeltaApplyResult::Malformed;
    const bool hasBaseline = (part.flags & DeltaSnapshot::kHasBaseline) != 0;
    const uint32_t tick = part.snapshotTick;
    if (m_seen && !tickNewer(tick, m_newestSeen) && m_newestSeen - tick >= kDeltaHistoryTicks)
        return DeltaApplyResult::Stale;

    const size_t index = tick % kDeltaHistoryTicks;
    Entry& entry = m_entries[index];
    NetEntityState* slots = &m_slots[index * m_slotCount];
    const bool sameTick = entry.used && entry.tick == tick;
    if (entry.used && !sameTick && !tickNewer(tick, entry.tick)) return DeltaApplyResult::Stale;
    if (sameTick) {
        if (entry.completeSnapshot) return DeltaApplyResult::Duplicate;
        if (entry.partCount != part.partCount || entry.hasBaseline != hasBaseline ||
            (hasBaseline && entry.baselineTick != part.baselineTick))
            return DeltaApplyResult::Malformed;
        if (entry.received[part.partIndex]) return DeltaApplyResult::Duplicate;
        // Parts of one snapshot cover separate slots.
        for (uint32_t i = 0; i < 255; ++i) {
            if (!entry.received[i]) continue;
            const uint32_t first = entry.ranges[i] >> 16, count = entry.ranges[i] & 0xFFFF;
            if (part.slotCount && count && part.firstSlot < first + count && first < part.firstSlot + part.slotCount)
                return DeltaApplyResult::Malformed;
        }
    }
    const NetEntityState* baseline = nullptr;
    if (!sameTick && hasBaseline) {
        baseline = complete(part.baselineTick);
        if (!baseline) return DeltaApplyResult::MissingBaseline;
    }

    // Decode into staging first, so a bad part changes nothing. A new snapshot's slots
    // start as its baseline's (or empty); a later part's slots are still untouched.
    const NetEntityState* base = sameTick ? slots : baseline;
    BitReader reader(part.payload);
    uint32_t staged = 0;
    for (uint32_t i = 0; i < part.slotCount; ++i) {
        bool changed = false;
        if (!reader.bit(changed)) return DeltaApplyResult::Malformed;
        if (!changed) continue;
        const uint16_t slot = uint16_t(part.firstSlot + i);
        if (staged == part.changedCount) return DeltaApplyResult::Malformed;
        if (!readEntity(reader, base ? base[slot] : kEmpty, m_stagedStates[staged])) return DeltaApplyResult::Malformed;
        m_stagedSlots[staged++] = slot;
    }
    if (staged != part.changedCount || !reader.paddedEnd()) return DeltaApplyResult::Malformed;

    if (!sameTick) {
        if (baseline) {
            if (baseline != slots) std::memcpy(slots, baseline, sizeof(NetEntityState) * m_slotCount);
        } else {
            std::fill_n(slots, m_slotCount, kEmpty);
        }
        entry.tick = tick;
        entry.used = true;
        entry.completeSnapshot = false;
        entry.hasBaseline = hasBaseline;
        entry.baselineTick = part.baselineTick;
        entry.partCount = part.partCount;
        entry.partsReceived = 0;
        entry.received.reset();
    }
    for (uint32_t i = 0; i < staged; ++i) slots[m_stagedSlots[i]] = m_stagedStates[i];
    entry.received[part.partIndex] = true;
    entry.ranges[part.partIndex] = uint32_t(part.firstSlot) << 16 | part.slotCount;
    ++entry.partsReceived;
    if (!m_seen || tickNewer(tick, m_newestSeen)) m_newestSeen = tick;
    m_seen = true;
    if (entry.partsReceived < entry.partCount) return DeltaApplyResult::Applied;
    entry.completeSnapshot = true;
    if (!m_hasComplete || tickNewer(tick, m_latestComplete)) m_latestComplete = tick;
    m_hasComplete = true;
    return DeltaApplyResult::Completed;
}

}   // namespace shockwave::net
