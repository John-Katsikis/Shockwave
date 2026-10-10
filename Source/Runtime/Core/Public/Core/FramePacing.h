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
};

struct FrameSummary {
    size_t frames = 0;
    double averageMs = 0, averageFps = 0;
    double low1Fps = 0, low01Fps = 0;   // fps at the slowest 1% / 0.1% of frames
    double maxMs = 0;   // the worst frame
    double jitterMs = 0;   // standard deviation of frame time
    double cpuMs = 0, gpuMs = 0, presentMs = 0;   // averages
};

class FrameStats {
public:
    explicit FrameStats(size_t capacity = 2048);
    void add(const FrameSample& sample);
    void clear();
    size_t size() const { return m_count; }
    // Oldest first.
    std::vector<FrameSample> samples() const;
    FrameSummary summary() const;

private:
    std::vector<FrameSample> m_ring;
    size_t m_next = 0, m_count = 0;
};

// The summary of any list of frame times, for tools that collect their own.
FrameSummary summarizeFrames(const std::vector<FrameSample>& samples);

}   // namespace shockwave
