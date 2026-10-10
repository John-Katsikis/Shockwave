// Frame pacing: the limiter's timing, the smoother, the frame statistics, and the
// frame_limit / low_latency project settings. Timing checks are loose, since CI
// machines are busy; they catch a broken limiter, not a slightly late one.
#include <Core/FramePacing.h>
#include <Projects/Project.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace shockwave;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static bool near(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

static void testLimiter() {
    using Clock = FrameLimiter::Clock;
    FrameLimiter limiter;
    const auto t0 = Clock::now();
    limiter.wait();   // no limit: returns at once
    CHECK(Clock::now() - t0 < std::chrono::milliseconds(5));

    limiter.setTargetFps(200.0);
    CHECK(limiter.targetFps() == 200.0);
    limiter.wait();   // starts the schedule
    const auto start = Clock::now();
    constexpr int frames = 40;
    for (int i = 0; i < frames; ++i) limiter.wait();
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    // 40 frames at 5 ms = 200 ms; absolute deadlines keep that from drifting.
    CHECK(seconds > 0.190 && seconds < 0.260);
    if (!(seconds > 0.190 && seconds < 0.260)) std::fprintf(stderr, "  40 frames took %.1f ms\n", seconds * 1000);

    // A long stall restarts the schedule rather than rushing frames to catch up.
    const auto stall = Clock::now() + std::chrono::milliseconds(30);
    while (Clock::now() < stall) {}
    limiter.wait();
    const auto afterStall = Clock::now();
    limiter.wait();
    CHECK(Clock::now() - afterStall > std::chrono::milliseconds(3));
}

static void testSmoother() {
    FrameTimeSmoother smoother;
    smoother.setPeriod(1.0 / 60.0);
    for (int i = 0; i < 10; ++i) smoother.smooth(1.0 / 60.0);
    // Close to the refresh period: snapped to it exactly.
    CHECK(smoother.smooth(0.0170) == 1.0 / 60.0);
    // One long hitch is clamped to twice the median, not passed through whole.
    CHECK(near(smoother.smooth(0.200), 2.0 / 60.0, 1e-9));
    // A steady new rate is followed once it becomes the median.
    double last = 0;
    for (int i = 0; i < 10; ++i) last = smoother.smooth(1.0 / 30.0);
    CHECK(near(last, 1.0 / 30.0, 1e-9));

    FrameTimeSmoother unsnapped;   // period unknown: values pass through
    for (int i = 0; i < 10; ++i) unsnapped.smooth(0.010);
    CHECK(near(unsnapped.smooth(0.011), 0.011, 1e-12));
}

static void testStats() {
    // 990 frames at 10 ms and 10 at 50 ms: the 1% low is the 10 slowest.
    std::vector<FrameSample> samples;
    for (int i = 0; i < 1000; ++i)
        samples.push_back({.cpuMs = 2, .gpuMs = 3, .frameMs = (i % 100 == 0) ? 50.0f : 10.0f, .presentMs = 0});
    const FrameSummary s = summarizeFrames(samples);
    CHECK(s.frames == 1000);
    CHECK(near(s.averageMs, 10.4, 1e-6));
    CHECK(near(s.averageFps, 1000.0 / 10.4, 1e-3));
    CHECK(near(s.low1Fps, 20.0, 1e-6));
    CHECK(near(s.low01Fps, 20.0, 1e-6));
    CHECK(near(s.maxMs, 50.0, 1e-6));
    CHECK(s.jitterMs > 3.0 && s.jitterMs < 5.0);   // sqrt(0.01 * 0.99) * 40 = 3.98
    CHECK(near(s.cpuMs, 2.0, 1e-6) && near(s.gpuMs, 3.0, 1e-6));

    CHECK(summarizeFrames({}).frames == 0);

    // The ring keeps the newest samples, oldest first.
    FrameStats ring(4);
    for (int i = 1; i <= 6; ++i) ring.add({.frameMs = static_cast<float>(i)});
    CHECK(ring.size() == 4);
    const auto kept = ring.samples();
    CHECK(kept.size() == 4 && kept.front().frameMs == 3.0f && kept.back().frameMs == 6.0f);
    CHECK(near(ring.summary().maxMs, 6.0, 1e-6));
    ring.clear();
    CHECK(ring.size() == 0 && ring.samples().empty());
}

static void testProjectSettings(const std::filesystem::path& source) {
    Project project;
    std::string error;
    CHECK(loadProject(source / "Games/Sample/Sample.swproject", project, error));
    if (!error.empty()) std::fprintf(stderr, "  %s\n", error.c_str());
    CHECK(project.frameLimit == 0 && !project.lowLatency);   // defaults

    // Save a copy beside the original, so its relative paths still resolve.
    const auto copy = source / "Games/Sample/FramePacingTest.swproject";
    project.file = copy;
    project.frameLimit = 144;
    project.lowLatency = true;
    CHECK(saveProject(project, error));
    Project again;
    CHECK(loadProject(copy, again, error));
    CHECK(again.frameLimit == 144 && again.lowLatency);
    std::filesystem::remove(copy);
}

int main(int argc, char** argv) {
    testLimiter();
    testSmoother();
    testStats();
    if (argc > 1) testProjectSettings(argv[1]);
    if (failures) {
        std::fprintf(stderr, "%d frame pacing check(s) failed\n", failures);
        return 1;
    }
    std::puts("frame pacing: all checks passed");
    return 0;
}
