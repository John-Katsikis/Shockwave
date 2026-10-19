// Remote-object interpolation and the client's server clock (Multiplayer/Interpolation.h).
#include <Multiplayer/Interpolation.h>

#include <Multiplayer/NetworkSettings.h>

#include <algorithm>
#include <cmath>

namespace shockwave::net {

namespace {

glm::vec3 vec(const NetVec3& v) { return {v.x, v.y, v.z}; }
glm::quat quat(const NetQuat& q) { return {q.w, q.x, q.y, q.z}; }

// Server ticks past `from`, as a signed count (ticks wrap).
double ticksAfter(uint32_t tick, uint32_t from) { return double(int32_t(tick - from)); }

}   // namespace

InterpolationSettings interpolationSettings(const NetworkSettings& s) {
    InterpolationSettings out;
    out.tickRate = std::max<uint32_t>(s.tickRate, 1);
    out.delayTicks = double(s.interpolationMs) * out.tickRate / 1000.0;
    out.maxExtrapolationTicks = double(s.maxExtrapolationMs) * out.tickRate / 1000.0;
    return out;
}

InterpolationEngine::InterpolationEngine(uint16_t slotCount, const InterpolationSettings& settings)
    : m_settings(settings), m_slotCount(std::clamp<uint16_t>(slotCount, 1, kMaxDeltaSlots)) {
    m_settings.samplesPerSlot = std::clamp<uint16_t>(m_settings.samplesPerSlot, 4, 64);
    m_settings.tickRate = std::max<uint32_t>(m_settings.tickRate, 1);
    m_samples = std::make_unique<Sample[]>(size_t(m_slotCount) * m_settings.samplesPerSlot);
    m_counts = std::make_unique<uint16_t[]>(m_slotCount);
}

void InterpolationEngine::clear(uint16_t slot) {
    if (slot < m_slotCount) m_counts[slot] = 0;
}

uint16_t InterpolationEngine::sampleCount(uint16_t slot) const { return slot < m_slotCount ? m_counts[slot] : 0; }

void InterpolationEngine::push(uint16_t slot, uint32_t tick, const NetEntityState& state) {
    if (slot >= m_slotCount) return;
    if (!state.active()) {
        clear(slot);
        return;
    }
    Sample* samples = &m_samples[size_t(slot) * m_settings.samplesPerSlot];
    uint16_t& count = m_counts[slot];
    const PoseState pose = dequantizeState(state);
    if (count > 0) {
        const Sample& newest = samples[count - 1];
        const bool newer = tickNewer(tick, newest.tick);
        const bool discontinuity = state.incarnation != newest.incarnation || (state.flags & PoseState::kTeleport);
        // A respawn or teleport starts over; a late sample from either side of one is dropped.
        if (discontinuity && !newer) return;
        if (discontinuity) count = 0;
        // Nothing from before the teleport that started these samples.
        if (!discontinuity && (samples[0].pose.flags & PoseState::kTeleport) && tickNewer(samples[0].tick, tick))
            return;
    }
    // Sorted insert, oldest first; when full the oldest goes.
    uint16_t at = count;
    while (at > 0 && tickNewer(samples[at - 1].tick, tick)) --at;
    if (at > 0 && samples[at - 1].tick == tick) return;   // duplicate
    if (count == m_settings.samplesPerSlot) {
        if (at == 0) return;   // older than everything kept
        // The oldest goes: the ones before the new sample move down a place.
        std::move(samples + 1, samples + at, samples);
        samples[at - 1] = {tick, pose, state.incarnation};
        return;
    }
    std::move_backward(samples + at, samples + count, samples + count + 1);
    samples[at] = {tick, pose, state.incarnation};
    ++count;
}

void InterpolationEngine::pushSnapshot(uint32_t tick, std::span<const NetEntityState> slots) {
    const size_t n = std::min<size_t>(slots.size(), m_slotCount);
    for (size_t i = 0; i < n; ++i) {
        if (slots[i].active() || m_counts[i]) push(uint16_t(i), tick, slots[i]);
    }
}

void InterpolationEngine::observeServerTick(uint32_t tick, double nowSeconds) {
    const double rate = m_settings.tickRate;
    if (!m_hasClock) {
        m_hasClock = true;
        m_clockBase = tick;
        m_offset = -nowSeconds * rate;
        return;
    }
    const double measured = ticksAfter(tick, m_clockBase) - nowSeconds * rate;
    if (measured > m_offset || m_offset - measured > rate * 0.5)
        m_offset = measured;   // an earlier arrival than any so far, or the route changed
    else
        m_offset += (measured - m_offset) * 0.02;   // drift slowly towards typical arrivals
}

NetTime InterpolationEngine::renderTime(double nowSeconds) const {
    if (!m_hasClock) return {};
    const double server = m_offset + nowSeconds * m_settings.tickRate - m_settings.delayTicks;
    const double whole = std::floor(server);
    return {uint32_t(m_clockBase + int64_t(whole)), float(server - whole)};
}

InterpolationResult InterpolationEngine::sample(uint16_t slot, NetTime time, InterpolatedPose& out) const {
    if (slot >= m_slotCount || m_counts[slot] == 0) return InterpolationResult::Empty;
    const Sample* samples = &m_samples[size_t(slot) * m_settings.samplesPerSlot];
    const uint16_t count = m_counts[slot];
    const double rate = m_settings.tickRate;
    // Each sample's time relative to the one asked for, in ticks.
    auto when = [&](const Sample& s) { return ticksAfter(s.tick, time.tick) - time.fraction; };
    auto fill = [&](const Sample& s) {
        out.position = vec(s.pose.position);
        out.rotation = quat(s.pose.rotation);
        out.velocity = vec(s.pose.linearVelocity);
        out.angularVelocity = vec(s.pose.angularVelocity);
        out.incarnation = s.incarnation;
    };

    uint16_t after = 0;   // the first sample later than the time
    while (after < count && when(samples[after]) <= 0) ++after;
    if (after == count) {
        const Sample& newest = samples[count - 1];
        const double ahead = -when(newest);
        fill(newest);
        if (ahead <= 0) return InterpolationResult::Interpolated;
        const double used = std::min(ahead, m_settings.maxExtrapolationTicks);
        if (!(newest.pose.flags & PoseState::kSleeping)) out.position += out.velocity * float(used / rate);
        return ahead <= m_settings.maxExtrapolationTicks ? InterpolationResult::Extrapolated
                                                         : InterpolationResult::Held;
    }
    if (after == 0) {
        fill(samples[0]);
        return InterpolationResult::Held;
    }
    const Sample& a = samples[after - 1];
    const Sample& b = samples[after];
    const double span = when(b) - when(a);
    const float s = float(-when(a) / span);
    const glm::vec3 pa = vec(a.pose.position), pb = vec(b.pose.position);
    const glm::vec3 va = vec(a.pose.linearVelocity), vb = vec(b.pose.linearVelocity);
    if (m_settings.hermite) {
        const float dt = float(span / rate);
        const float s2 = s * s, s3 = s2 * s;
        out.position =
            (2 * s3 - 3 * s2 + 1) * pa + (s3 - 2 * s2 + s) * dt * va + (-2 * s3 + 3 * s2) * pb + (s3 - s2) * dt * vb;
    } else {
        out.position = glm::mix(pa, pb, s);
    }
    out.rotation = glm::slerp(quat(a.pose.rotation), quat(b.pose.rotation), s);
    out.velocity = glm::mix(va, vb, s);
    out.angularVelocity = glm::mix(vec(a.pose.angularVelocity), vec(b.pose.angularVelocity), s);
    out.incarnation = b.incarnation;
    return InterpolationResult::Interpolated;
}

}   // namespace shockwave::net
