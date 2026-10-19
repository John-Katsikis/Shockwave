#pragma once

// Client-side presentation of remote objects (docs/SteamMultiplayerImplementation.md
// section 9.3): every complete snapshot adds a sample per slot, and each frame shows the
// objects at the estimated server time minus a fixed delay, the jitter buffer, so a late
// or lost snapshot still leaves a sample ahead to move towards.
//
// Positions blend with cubic Hermite curves through both samples' velocities (or linearly),
// rotations with the shortest slerp, velocities linearly. Past the newest sample an object
// keeps its velocity for up to max_extrapolation_ms, then holds and reports itself stale;
// nothing invents acceleration. A teleport, a new incarnation or an emptied slot clears
// the slot's samples. All storage is allocated in the constructor.

#include <Multiplayer/SnapshotDelta.h>

#include <glm/common.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <span>

namespace shockwave::net {

struct NetworkSettings;

struct InterpolationSettings {
    uint32_t tickRate = 60;
    double delayTicks = 6;   // how far behind the newest server time objects are shown
    double maxExtrapolationTicks = 6;
    uint16_t samplesPerSlot = 32;   // 4..64
    bool hermite = true;   // false: linear positions
};
// From the session's settings: interpolation_ms and max_extrapolation_ms in ticks.
// At tick_rate 120 with interpolation_ms 25 that is a 3-tick jitter buffer.
InterpolationSettings interpolationSettings(const NetworkSettings& settings);

// A point on the server timeline: a tick and how far into the next one.
struct NetTime {
    uint32_t tick = 0;
    float fraction = 0;   // 0..1
};

struct InterpolatedPose {
    glm::vec3 position{0.0f};
    glm::quat rotation{1, 0, 0, 0};
    glm::vec3 velocity{0.0f};
    glm::vec3 angularVelocity{0.0f};
    uint16_t incarnation = 0;
};

enum class InterpolationResult : uint8_t {
    Empty,   // no samples: nothing to show
    Interpolated,   // between two samples
    Extrapolated,   // past the newest sample, within max extrapolation
    Held,   // past max extrapolation, or before the oldest sample: the pose is stale
};

class InterpolationEngine {
public:
    InterpolationEngine(uint16_t slotCount, const InterpolationSettings& settings);
    InterpolationEngine(const InterpolationEngine&) = delete;
    InterpolationEngine& operator=(const InterpolationEngine&) = delete;

    const InterpolationSettings& settings() const { return m_settings; }
    uint16_t slotCount() const { return m_slotCount; }

    // A slot's state at a server tick. Samples may arrive in any order; duplicates and
    // samples older than everything kept are dropped.
    void push(uint16_t slot, uint32_t tick, const NetEntityState& state);
    // Every slot of a complete snapshot (SnapshotDeltaDecoder::complete).
    void pushSnapshot(uint32_t tick, std::span<const NetEntityState> slots);
    void clear(uint16_t slot);
    uint16_t sampleCount(uint16_t slot) const;

    // The server clock: a snapshot for `tick` arrived at local time `nowSeconds`. The
    // estimate follows the earliest arrivals, so late packets don't drag it back.
    void observeServerTick(uint32_t tick, double nowSeconds);
    bool hasClock() const { return m_hasClock; }
    // Where to show objects at `nowSeconds`: estimated server time minus the delay.
    NetTime renderTime(double nowSeconds) const;

    InterpolationResult sample(uint16_t slot, NetTime time, InterpolatedPose& out) const;

private:
    struct Sample {
        uint32_t tick;
        PoseState pose;
        uint16_t incarnation;
    };
    InterpolationSettings m_settings;
    uint16_t m_slotCount;
    std::unique_ptr<Sample[]> m_samples;   // slotCount x samplesPerSlot, oldest first per slot
    std::unique_ptr<uint16_t[]> m_counts;
    bool m_hasClock = false;
    uint32_t m_clockBase = 0;   // ticks are measured from here so doubles stay exact
    double m_offset = 0;   // server time (ticks since m_clockBase) minus local time in ticks
};

}   // namespace shockwave::net
