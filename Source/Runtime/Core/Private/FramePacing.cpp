#include <Core/FramePacing.h>

#include <algorithm>
#include <cmath>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace shockwave {

void FrameLimiter::setTargetFps(double fps) {
    m_fps = fps > 0.0 ? fps : 0.0;
    m_period = m_fps > 0.0 ? std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / m_fps))
                           : Clock::duration{};
    m_next = {};
}

FrameLimiter::Clock::time_point FrameLimiter::wait() {
    Clock::time_point now = Clock::now();
    if (m_fps <= 0.0) return now;
    if (m_next == Clock::time_point{} || now - m_next > m_period * 2) {
        // First frame, or we stalled: start a fresh schedule from now.
        m_next = now + m_period;
        return now;
    }
    if (now < m_next - m_spin) std::this_thread::sleep_until(m_next - m_spin);
    while ((now = Clock::now()) < m_next) {
#if defined(_MSC_VER) && defined(_M_X64)
        _mm_pause();
#elif defined(_MSC_VER) && defined(_M_ARM64)
        __yield();
#elif defined(__x86_64__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }
    m_next += m_period;
    return now;
}

double FrameTimeSmoother::smooth(double raw) {
    double value = raw;
    if (m_count >= 3) {
        std::array<double, 7> sorted{};
        const size_t n = std::min(m_count, m_history.size());
        std::copy_n(m_history.begin(), n, sorted.begin());
        std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.begin() + n);
        const double median = sorted[n / 2];
        value = std::clamp(raw, median * 0.5, median * 2.0);
    }
    if (m_period > 0.0 && std::abs(value - m_period) < m_period * 0.05) value = m_period;
    m_history[m_next] = raw;
    m_next = (m_next + 1) % m_history.size();
    ++m_count;
    return value;
}

FrameStats::FrameStats(size_t capacity) : m_ring(std::max<size_t>(capacity, 1)) {}

void FrameStats::add(const FrameSample& sample) {
    m_ring[m_next] = sample;
    m_next = (m_next + 1) % m_ring.size();
    m_count = std::min(m_count + 1, m_ring.size());
}

void FrameStats::clear() {
    m_next = 0;
    m_count = 0;
}

std::vector<FrameSample> FrameStats::samples() const {
    std::vector<FrameSample> out;
    out.reserve(m_count);
    const size_t start = (m_next + m_ring.size() - m_count) % m_ring.size();
    for (size_t i = 0; i < m_count; ++i) out.push_back(m_ring[(start + i) % m_ring.size()]);
    return out;
}

FrameSample FrameStats::latest() const {
    return m_count ? m_ring[(m_next + m_ring.size() - 1) % m_ring.size()] : FrameSample{};
}

FrameSummary FrameStats::summary() const { return summarizeFrames(samples()); }

double framePercentile(std::vector<double> times, double percent) {
    if (times.empty()) return 0;
    percent = std::clamp(percent, 0.0, 100.0);
    std::sort(times.begin(), times.end());
    const double n = static_cast<double>(times.size());
    const size_t rank = static_cast<size_t>(std::ceil(percent / 100.0 * n - 1e-9));
    return times[std::clamp<size_t>(rank, 1, times.size()) - 1];
}

FrameSummary summarizeFrames(const std::vector<FrameSample>& samples) {
    FrameSummary s;
    s.frames = samples.size();
    if (samples.empty()) return s;
    std::vector<double> times;
    times.reserve(samples.size());
    std::vector<double> presents;
    double total = 0, cpu = 0, gpu = 0, present = 0, simulation = 0, game = 0, submit = 0;
    size_t gpuCount = 0, presentCount = 0;
    for (const FrameSample& f : samples) {
        times.push_back(f.frameMs);
        total += f.frameMs;
        cpu += f.cpuMs;
        simulation += f.simulationMs;
        game += f.gameMs;
        submit += f.submitMs;
        if (f.gpuMs > 0) gpu += f.gpuMs, ++gpuCount;
        if (f.presentMs > 0) present += f.presentMs, ++presentCount, presents.push_back(f.presentMs);
    }
    const double n = static_cast<double>(samples.size());
    s.averageMs = total / n;
    s.averageFps = s.averageMs > 0 ? 1000.0 / s.averageMs : 0;
    s.cpuMs = cpu / n;
    s.simulationMs = simulation / n;
    s.gameMs = game / n;
    s.submitMs = submit / n;
    s.p50Ms = framePercentile(times, 50);
    s.p95Ms = framePercentile(times, 95);
    s.p99Ms = framePercentile(times, 99);
    s.presentFrames = presentCount;
    if (presentCount == samples.size()) {
        s.presentP95Ms = framePercentile(presents, 95);
        s.presentP99Ms = framePercentile(presents, 99);
    }
    s.gpuMs = gpuCount ? gpu / static_cast<double>(gpuCount) : 0;
    s.presentMs = presentCount ? present / static_cast<double>(presentCount) : 0;
    double variance = 0;
    for (double t : times) variance += (t - s.averageMs) * (t - s.averageMs);
    s.jitterMs = std::sqrt(variance / n);
    // "1% low": the average frame rate of the slowest 1% of frames (at least one frame).
    std::sort(times.begin(), times.end(), std::greater<>());
    s.maxMs = times.front();
    auto low = [&](double fraction) {
        const size_t count = std::max<size_t>(1, static_cast<size_t>(std::floor(n * fraction)));
        double sum = 0;
        for (size_t i = 0; i < count; ++i) sum += times[i];
        const double ms = sum / static_cast<double>(count);
        return ms > 0 ? 1000.0 / ms : 0.0;
    };
    s.low1Fps = low(0.01);
    s.low01Fps = low(0.001);
    return s;
}

}   // namespace shockwave
