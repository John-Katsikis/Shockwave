// Lag-compensated hit checks against recorded player capsules (Multiplayer/RewindHistory.h).
#include <Multiplayer/RewindHistory.h>

#include <Multiplayer/NetworkSettings.h>

#include <Network/Wire.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace shockwave::net {

const char* rewindStatusName(RewindStatus status) {
    switch (status) {
    case RewindStatus::Exact: return "exact";
    case RewindStatus::Clamped: return "clamped";
    case RewindStatus::Rejected: return "rejected";
    case RewindStatus::Empty: return "empty";
    }
    return "unknown";
}

RewindHistoryBuffer::RewindHistoryBuffer(
    uint16_t playerSlots, uint32_t tickRate, uint32_t maxRewindMs, uint32_t toleranceTicks)
    : m_slots(std::max<uint16_t>(playerSlots, 1)), m_tickRate(std::max<uint32_t>(tickRate, 1)),
      m_toleranceTicks(toleranceTicks) {
    const uint64_t rewind = (uint64_t(maxRewindMs) * m_tickRate + 999) / 1000;
    m_capacity = uint16_t(std::clamp<uint64_t>(rewind + 2, 3, kMaxRewindSamples));
    m_maxRewindTicks = std::min<uint32_t>(uint32_t(rewind), m_capacity - 2u);
    // Stored 32 per slot whatever the window, so tick & 31 finds a tick even across the
    // tick counter wrapping (a modulus that doesn't divide 2^32 would collide there).
    m_samples = std::make_unique<Sample[]>(size_t(m_slots) * kMaxRewindSamples);
    m_scratch = std::make_unique<RewindHitbox[]>(m_slots);
}

RewindHistoryBuffer::RewindHistoryBuffer(const NetworkSettings& s)
    : RewindHistoryBuffer(uint16_t(s.maxPlayers), s.tickRate, s.maxRewindMs) {}

bool RewindHistoryBuffer::record(uint32_t tick, std::span<const RewindPlayerState> players) {
    if (players.size() != m_slots) return false;
    if (m_recorded && !tickNewer(tick, m_latest)) return false;
    const size_t index = tick & (kMaxRewindSamples - 1);
    for (uint16_t i = 0; i < m_slots; ++i) {
        Sample& s = m_samples[size_t(i) * kMaxRewindSamples + index];
        s.state = players[i];
        s.tick = tick;
        s.used = true;
    }
    m_latest = tick;
    m_recorded = true;
    return true;
}

size_t RewindHistoryBuffer::memoryBytes() const {
    return size_t(m_slots) * (kMaxRewindSamples * sizeof(Sample) + sizeof(RewindHitbox));
}

const RewindHistoryBuffer::Sample* RewindHistoryBuffer::at(uint16_t slot, uint32_t tick) const {
    if (tickNewer(tick, m_latest) || m_latest - tick >= m_capacity) return nullptr;
    const Sample& s = m_samples[size_t(slot) * kMaxRewindSamples + (tick & (kMaxRewindSamples - 1))];
    return s.used && s.tick == tick ? &s : nullptr;
}

uint64_t RewindHistoryBuffer::timestampMs(uint32_t tick) const {
    return (uint64_t(tick) * 1000 + m_tickRate / 2) / m_tickRate;
}

// `ticksAfterLatest` is the time asked for relative to the latest tick (<= 0 is the past).
RewindQuery RewindHistoryBuffer::resolve(double ticksAfterLatest) const {
    RewindQuery q;
    if (!m_recorded) return q;
    q.status = RewindStatus::Exact;
    double t = ticksAfterLatest;
    const double oldest = -double(m_maxRewindTicks);
    if (!(t <= 0)) {   // the future (or NaN): now
        t = 0;
        q.status = RewindStatus::Clamped;
    } else if (t < oldest) {
        if (t < oldest - double(m_toleranceTicks)) {
            q.status = RewindStatus::Rejected;
            return q;
        }
        t = oldest;
        q.status = RewindStatus::Clamped;
    }
    const double whole = std::floor(t);
    q.tick = m_latest + uint32_t(int64_t(whole));
    q.fraction = float(t - whole);
    if (q.fraction >= 1.0f) {   // rounding just below a whole tick
        q.fraction = 0;
        ++q.tick;
    }
    return q;
}

void RewindHistoryBuffer::fill(const RewindQuery& q, std::span<RewindHitbox> out) const {
    const size_t n = std::min<size_t>(out.size(), m_slots);
    const bool usable = q.status == RewindStatus::Exact || q.status == RewindStatus::Clamped;
    for (size_t i = 0; i < n; ++i) {
        RewindHitbox& box = out[i];
        box = {};
        if (!usable) continue;
        const Sample* a = at(uint16_t(i), q.tick);
        const Sample* b = q.fraction > 0 ? at(uint16_t(i), q.tick + 1) : a;
        if (!a || !b) continue;
        const RewindPlayerState& sa = a->state;
        const RewindPlayerState& sb = b->state;
        // One life, alive at both ends, and no teleport in between.
        if (!sa.incarnation || sa.incarnation != sb.incarnation) continue;
        if (!(sa.flags & RewindPlayerState::kAlive) || !(sb.flags & RewindPlayerState::kAlive)) continue;
        if (b != a && (sb.flags & RewindPlayerState::kTeleported)) continue;
        const float f = q.fraction;
        const glm::vec3 feet = sa.feet + (sb.feet - sa.feet) * f;
        const float radius = sa.radius + (sb.radius - sa.radius) * f;
        const float height = std::max(sa.height + (sb.height - sa.height) * f, 2 * radius);
        box.a = feet + glm::vec3(0, radius, 0);
        box.b = feet + glm::vec3(0, height - radius, 0);
        box.eye = sa.eye + (sb.eye - sa.eye) * f;
        box.radius = radius;
        box.incarnation = sa.incarnation;
        box.team = sb.team;
        box.flags = sb.flags;
        box.valid = true;
    }
}

RewindQuery RewindHistoryBuffer::queryAtTimestamp(uint64_t clientTimestampMs, std::span<RewindHitbox> out) const {
    const double ticks = double(clientTimestampMs) * m_tickRate / 1000.0;
    const RewindQuery q = resolve(ticks - double(m_latest));
    fill(q, out);
    return q;
}

RewindQuery RewindHistoryBuffer::queryAtTick(uint32_t tick, float fraction, std::span<RewindHitbox> out) const {
    const RewindQuery q = resolve(double(int32_t(tick - m_latest)) + std::clamp(fraction, 0.0f, 1.0f));
    fill(q, out);
    return q;
}

float rayCapsule(const glm::vec3& o, const glm::vec3& d, const glm::vec3& a, const glm::vec3& b, float r) {
    const glm::vec3 ba = b - a, oa = o - a;
    const float baba = glm::dot(ba, ba);
    // Inside already?
    const float h0 = baba > 0 ? std::clamp(glm::dot(oa, ba) / baba, 0.0f, 1.0f) : 0.0f;
    const glm::vec3 nearest = oa - ba * h0;
    if (glm::dot(nearest, nearest) <= r * r) return 0;
    // The side: an infinite cylinder, kept where it lies between the ends.
    const float bard = glm::dot(ba, d), baoa = glm::dot(ba, oa);
    const float A = baba - bard * bard;
    if (A > 1e-9f * std::max(baba, 1.0f)) {
        const float B = baba * glm::dot(d, oa) - baoa * bard;
        const float C = baba * glm::dot(oa, oa) - baoa * baoa - r * r * baba;
        const float h = B * B - A * C;
        if (h >= 0) {
            const float t = (-B - std::sqrt(h)) / A;
            const float y = baoa + t * bard;
            if (y > 0 && y < baba) return t >= 0 ? t : -1.0f;
        }
    }
    // The end caps.
    float best = -1.0f;
    for (const glm::vec3& c : {a, b}) {
        const glm::vec3 oc = o - c;
        const float bq = glm::dot(d, oc);
        const float h = bq * bq - (glm::dot(oc, oc) - r * r);
        if (h < 0) continue;
        const float t = -bq - std::sqrt(h);
        if (t >= 0 && (best < 0 || t < best)) best = t;
    }
    return best;
}

bool RewindHistoryBuffer::cast(
    const RewindQuery& q, const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint16_t shooterSlot,
    RewindHit& hit, RewindQuery* query) const {
    if (query) *query = q;
    fill(q, std::span<RewindHitbox>(m_scratch.get(), m_slots));
    bool found = false;
    for (uint16_t i = 0; i < m_slots; ++i) {
        const RewindHitbox& box = m_scratch[i];
        if (!box.valid || i == shooterSlot) continue;
        const float t = rayCapsule(origin, direction, box.a, box.b, box.radius);
        if (t < 0 || t > maxDistance || (found && t >= hit.distance)) continue;
        hit = {i, t, origin + direction * t, box.incarnation, box.team, box.flags};
        found = true;
    }
    return found;
}

bool RewindHistoryBuffer::raycastAtTick(
    const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t tick, float fraction,
    uint16_t shooterSlot, RewindHit& hit, RewindQuery* query) const {
    const RewindQuery q = resolve(double(int32_t(tick - m_latest)) + std::clamp(fraction, 0.0f, 1.0f));
    return cast(q, origin, direction, maxDistance, shooterSlot, hit, query);
}

bool RewindHistoryBuffer::raycast(
    const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint64_t clientTimestampMs,
    uint16_t shooterSlot, RewindHit& hit, RewindQuery* query) const {
    const RewindQuery q = resolve(double(clientTimestampMs) * m_tickRate / 1000.0 - double(m_latest));
    return cast(q, origin, direction, maxDistance, shooterSlot, hit, query);
}

}   // namespace shockwave::net
