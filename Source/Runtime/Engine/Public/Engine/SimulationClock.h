#pragma once

// SimulationClock: how much real time becomes fixed simulation ticks each frame.
//
// Offline (Smoothed, the default) it is what the engine loop always did: the frame time
// after FrameTimeSmoother's visual smoothing, at most 0.25 s a frame, and as many ticks
// as that buys. A long stall slows the game instead of running a giant step.
//
// Online (Monotonic) peers must agree on time, so smoothing would drift: the clock uses
// the raw monotonic frame time instead. It runs at most kMaxCatchUpTicks ticks a frame
// and carries the rest over as debt. A frame longer than kStall (a stall), or debt past
// kMaxDebt, is an overrun, which ends network play (SimulationOverrun) rather than
// catching up with a burst of ticks. The debt is then dropped so the next frame starts
// clean. See docs/Networking.md, "Session integration".

#include <cstdint>

namespace shockwave {

enum class SimulationTime : uint8_t {
    Smoothed,   // offline: smoothed frame time, clamped
    Monotonic,   // online: raw monotonic time, bounded catch-up
};

const char* simulationTimeName(SimulationTime time);

// What one frame of the clock decided.
struct SimulationStep {
    int ticks = 0;   // fixed ticks to run this frame
    double frameTime = 0;   // what onFrame gets, in seconds
    double debt = 0;   // simulation time still owed after the ticks, in seconds
    bool stalled = false;   // Monotonic: the frame took longer than kStall
    bool overrun = false;   // Monotonic: a stall, or debt past kMaxDebt (the debt was dropped)
};

class SimulationClock {
public:
    static constexpr double kMaxFrameTime = 0.25;
    static constexpr int kMaxCatchUpTicks = 8;
    static constexpr double kStall = 0.25;
    static constexpr double kMaxDebt = 0.25;

    void setSource(SimulationTime source) { m_source = source; }
    SimulationTime source() const { return m_source; }

    // rawFrameTime is the real time since the last frame; smoothedFrameTime is
    // FrameTimeSmoother's version of it; dt is the fixed tick. Negative or non-finite
    // times count as 0.
    SimulationStep advance(double rawFrameTime, double smoothedFrameTime, double dt);

    // How far into the next tick we are, 0..1, for rendering.
    double alpha(double dt) const;
    double accumulator() const { return m_accumulator; }
    void reset() { m_accumulator = 0; }

    struct Counters {
        uint64_t stalls = 0, overruns = 0, cappedFrames = 0;   // cappedFrames: hit kMaxCatchUpTicks
    };
    const Counters& counters() const { return m_counters; }

private:
    SimulationTime m_source = SimulationTime::Smoothed;
    double m_accumulator = 0;
    Counters m_counters;
};

}   // namespace shockwave
