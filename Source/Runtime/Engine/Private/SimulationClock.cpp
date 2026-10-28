#include <Engine/SimulationClock.h>

#include <algorithm>
#include <cmath>

namespace shockwave {

const char* simulationTimeName(SimulationTime time) {
    switch (time) {
    case SimulationTime::Smoothed: return "smoothed";
    case SimulationTime::Monotonic: return "monotonic";
    case SimulationTime::Fixed: return "fixed";
    }
    return "?";
}

namespace {
double sane(double seconds) { return std::isfinite(seconds) && seconds > 0 ? seconds : 0.0; }
}   // namespace

SimulationStep SimulationClock::advance(double rawFrameTime, double smoothedFrameTime, double dt) {
    SimulationStep step;
    const double raw = sane(rawFrameTime);
    if (!(dt > 0) || !std::isfinite(dt)) return step;
    if (m_source == SimulationTime::Fixed) {
        m_accumulator = 0;
        step.frameTime = dt;
        step.ticks = 1;
        return step;
    }
    if (m_source == SimulationTime::Smoothed) {
        // Exactly the loop the engine always ran: subtract one tick at a time.
        step.frameTime = std::min(sane(smoothedFrameTime), kMaxFrameTime);
        m_accumulator += step.frameTime;
        while (m_accumulator >= dt) {
            m_accumulator -= dt;
            ++step.ticks;
        }
        step.debt = m_accumulator;
        return step;
    }
    step.frameTime = std::min(raw, kMaxFrameTime);
    step.stalled = raw > kStall;
    m_counters.stalls += step.stalled;
    m_accumulator += raw;
    while (m_accumulator >= dt && step.ticks < kMaxCatchUpTicks) {
        m_accumulator -= dt;
        ++step.ticks;
    }
    if (m_accumulator >= dt) ++m_counters.cappedFrames;
    step.debt = m_accumulator;
    // A stall can't be caught up honestly either: until sessions can resynchronize, it
    // ends network play the same way.
    if (step.stalled || m_accumulator > kMaxDebt) {
        step.overrun = true;
        ++m_counters.overruns;
        m_accumulator = std::fmod(m_accumulator, dt);
    }
    return step;
}

double SimulationClock::alpha(double dt) const { return dt > 0 ? std::clamp(m_accumulator / dt, 0.0, 1.0) : 1.0; }

}   // namespace shockwave
