#include <Network/Loopback.h>

#include <algorithm>

namespace shockwave::net {

uint64_t DeterministicRandom::next() {
    uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double DeterministicRandom::unit() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }

int64_t DeterministicRandom::range(int64_t lo, int64_t hi) {
    if (hi <= lo) return lo;
    const uint64_t span = uint64_t(hi - lo) + 1;
    return lo + int64_t(next() % span);
}

namespace {

constexpr FaultPreset kPresets[] = {
    {"clean", {}, 3600},
    {"cafe", {30000, 10000, 0.01, 0.002, 0.01}, 7200},
    {"poor", {75000, 25000, 0.05, 0.01, 0.05}, 7200},
    {"hostile", {200000, 50000, 0.10, 0.02, 0.10}, 3600},
};

}   // namespace

std::span<const FaultPreset> faultPresets() { return kPresets; }

const FaultPreset* findFaultPreset(std::string_view name) {
    for (const FaultPreset& preset : kPresets)
        if (name == preset.name) return &preset;
    return nullptr;
}

void FaultLink::addOutage(uint64_t startUs, uint64_t durationUs) {
    m_outages[m_nextOutage] = {startUs, startUs + durationUs};
    m_nextOutage = (m_nextOutage + 1) % m_outages.size();
}

bool FaultLink::inOutage(uint64_t us, uint64_t& endUs) const {
    bool any = false;
    for (const auto& [start, end] : m_outages) {
        if (us >= start && us < end) {
            endUs = any ? std::max(endUs, end) : end;
            any = true;
        }
    }
    return any;
}

FaultLink::Arrivals FaultLink::schedule(uint64_t nowUs, uint32_t bytes, Delivery delivery) {
    ++m_counters.messages;
    Arrivals out;
    const bool reliable = delivery == Delivery::ReliableOrdered;
    uint64_t outageEnd = 0;
    const bool outage = inOutage(nowUs, outageEnd);
    if (!reliable && outage) {
        ++m_counters.dropped;
        return out;
    }

    // Bandwidth: messages leave one after another at the link's rate.
    uint64_t departUs = nowUs;
    if (m_profile.bytesPerSecond) {
        const uint64_t start = std::max(nowUs, m_linkFreeUs);
        const uint64_t done =
            start + (uint64_t(bytes) * 1000000 + m_profile.bytesPerSecond - 1) / m_profile.bytesPerSecond;
        if (!reliable && done - nowUs > m_profile.maxQueueDelayUs) {
            ++m_counters.throttleDropped;
            return out;
        }
        m_linkFreeUs = done;
        departUs = done;
    }

    auto arrival = [&] {
        int64_t at = int64_t(departUs) + m_profile.latencyUs;
        if (m_profile.jitterUs) at += m_random.range(-int64_t(m_profile.jitterUs), m_profile.jitterUs);
        return uint64_t(std::max<int64_t>(at, int64_t(nowUs)));
    };

    if (reliable) {
        uint64_t at = arrival();
        const uint32_t retry = m_profile.retryUs ? m_profile.retryUs : 2 * m_profile.latencyUs + 20000;
        for (int attempt = 0; attempt < 8 && m_profile.reliableLoss > 0 && m_random.unit() < m_profile.reliableLoss;
             ++attempt) {
            at += retry;
            ++m_counters.retries;
        }
        if (outage) at = std::max(at, outageEnd + m_profile.latencyUs);
        // In order: never before the reliable message sent ahead of it.
        at = std::max(at, m_lastReliableUs);
        m_lastReliableUs = at;
        out.copies = 1;
        out.atUs[0] = at;
        return out;
    }

    if (m_profile.unreliableLoss > 0 && m_random.unit() < m_profile.unreliableLoss) {
        ++m_counters.dropped;
        return out;
    }
    out.copies = 1;
    out.atUs[0] = arrival();
    if (m_profile.duplication > 0 && m_random.unit() < m_profile.duplication) {
        out.copies = 2;
        out.atUs[1] = arrival();
        ++m_counters.duplicated;
    }
    return out;
}

}   // namespace shockwave::net
