#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace shockwave {

// Holds frames to a steady rate. Sleeping alone is only accurate to a millisecond or so,
// so wait() sleeps until shortly before the deadline and spins the rest. Deadlines are
// absolute (start + n * period), so small errors don't add up into drift; after a long
// stall the schedule restarts instead of rushing frames to catch up.
//
// Main thread; one per loop.
class FrameLimiter {
public:
    using Clock = std::chrono::steady_clock;

    // 0 or less: no limit, wait() returns at once.
    void setTargetFps(double fps);
    double targetFps() const { return m_fps; }
    // How long before a deadline to stop sleeping and spin. Larger is steadier and burns
    // more CPU; 1 ms suits Linux, Windows and macOS schedulers.
    void setSpinThreshold(std::chrono::microseconds spin) { m_spin = spin; }

    // Blocks until the next frame is due. Returns the time it returned at.
    Clock::time_point wait();

private:
    double m_fps = 0.0;
    Clock::duration m_period{};
    Clock::time_point m_next{};
    std::chrono::microseconds m_spin{1000};
};

// Turns raw frame times into the delta the game sees. A stutter of one long frame would
// make interpolated motion jump, so a frame far off the recent median is clamped towards
// it, and frames within a few percent of the display or limiter period are snapped to it,
// so motion advances by exactly one refresh per refresh.
class FrameTimeSmoother {
public:
    void setPeriod(double seconds) { m_period = seconds; }   // 0: unknown, don't snap
    double smooth(double rawSeconds);
    void reset() { m_count = 0; }

private:
    std::array<double, 7> m_history{};
    size_t m_count = 0, m_next = 0;
    double m_period = 0.0;
};

// Frame time statistics over a sliding window: averages hide stutter, so this keeps the
// slow tail (1% and 0.1% lows) that players actually feel.
struct FrameSample {
    float cpuMs = 0;   // the frame's work on the main thread, waits excluded
    float gpuMs = 0;   // the GPU's time for the frame, 0 if unknown
    float frameMs = 0;   // start of one frame to the start of the next
    float presentMs = 0;   // between two presents reaching the screen, 0 if unknown
    // Where the CPU time went (S10, docs/Performance.md): the fixed simulation ticks, the
    // game's own frame (input, pump, onFrame) and gathering plus submitting the draws. 0 if
    // not measured. They are parts of cpuMs, not extra time.
    float simulationMs = 0;
    float gameMs = 0;
    float submitMs = 0;
};

struct FrameSummary {
    size_t frames = 0;
    double averageMs = 0, averageFps = 0;
    double low1Fps = 0, low01Fps = 0;   // fps at the slowest 1% / 0.1% of frames
    double maxMs = 0;   // the worst frame
    double jitterMs = 0;   // standard deviation of frame time
    double cpuMs = 0, gpuMs = 0, presentMs = 0;   // averages
    double simulationMs = 0, gameMs = 0, submitMs = 0;   // averages of the CPU parts
    // Frame-time percentiles (nearest rank): half the frames took at most p50Ms, 95% at
    // most p95Ms. Release budgets are written in these (docs/Performance.md), since an
    // average hides the slow frames players feel.
    double p50Ms = 0, p95Ms = 0, p99Ms = 0;
    // The same over the intervals between frames reaching the screen, when every frame had
    // one (presentFrames == frames); otherwise 0 and the frame intervals stand in.
    size_t presentFrames = 0;
    double presentP95Ms = 0, presentP99Ms = 0;
};

// The nearest-rank percentile of some times: the smallest t with at least `percent` of
// them <= t. 0 for none. percent is clamped to 0..100.
double framePercentile(std::vector<double> times, double percent);

class FrameStats {
public:
    explicit FrameStats(size_t capacity = 2048);
    void add(const FrameSample& sample);
    void clear();
    size_t size() const { return m_count; }
    // Oldest first.
    std::vector<FrameSample> samples() const;
    // The newest frame; a zero sample when there are none.
    FrameSample latest() const;
    FrameSummary summary() const;

private:
    std::vector<FrameSample> m_ring;
    size_t m_next = 0, m_count = 0;
};

// The summary of any list of frame times, for tools that collect their own.
FrameSummary summarizeFrames(const std::vector<FrameSample>& samples);

}   // namespace shockwave
