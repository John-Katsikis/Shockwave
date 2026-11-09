// Performance budgets and records (S10, docs/Performance.md): frame-time percentiles, the
// release contract's version 2 budgets (and version 1 still reading), .swperf records
// round-tripping and refusing what they don't know, and the performance gate: which
// records count, which pass and why the rest don't. No window, GPU or game module.
#include <Core/FramePacing.h>
#include <Engine/SimulationClock.h>
#include <Projects/Performance.h>
#include <Projects/Project.h>
#include <Projects/ReleaseSettings.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace shockwave;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace {

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
bool anyContains(const std::vector<std::string>& lines, const std::string& part) {
    for (const std::string& line : lines)
        if (contains(line, part)) return true;
    return false;
}

void testPercentiles() {
    // Nearest rank: 1..100 ms, so p50 is 50, p95 95, p99 99, p100 100.
    std::vector<double> times;
    for (int i = 100; i >= 1; --i) times.push_back(i);
    CHECK(framePercentile(times, 50) == 50);
    CHECK(framePercentile(times, 95) == 95);
    CHECK(framePercentile(times, 99) == 99);
    CHECK(framePercentile(times, 100) == 100);
    CHECK(framePercentile(times, 0) == 1);
    CHECK(framePercentile({}, 95) == 0);
    CHECK(framePercentile({7}, 99) == 7);
    // One hitch in 100 frames sits at p100, never in p95: the budget for it is max_hitch_ms.
    std::vector<FrameSample> samples(100, FrameSample{2, 0, 10, 0, 0.5f, 1, 0.5f});
    samples[40].frameMs = 80;
    const FrameSummary s = summarizeFrames(samples);
    CHECK(s.p50Ms == 10 && s.p95Ms == 10 && s.p99Ms == 10 && s.maxMs == 80);
    CHECK(std::abs(s.simulationMs - 0.5) < 1e-6 && std::abs(s.gameMs - 1) < 1e-6 && std::abs(s.submitMs - 0.5) < 1e-6);
    // Present percentiles only when every frame had a present interval.
    CHECK(s.presentFrames == 0 && s.presentP95Ms == 0);
    for (FrameSample& f : samples) f.presentMs = 16;
    const FrameSummary presented = summarizeFrames(samples);
    CHECK(presented.presentFrames == 100 && presented.presentP95Ms == 16);
    // The newest sample.
    FrameStats stats(4);
    CHECK(stats.latest().frameMs == 0);
    for (int i = 1; i <= 6; ++i) stats.add({0, 0, float(i), 0});
    CHECK(stats.latest().frameMs == 6);
}

const char* kV1 = "shockwave_release 1\n"
                  "application_id \"com.example.game\"\n"
                  "release_version \"1.0.0\"\n"
                  "platform linux-x86_64\n"
                  "mode offline\n"
                  "max_players 1\n"
                  "target_fps 60\n"
                  "simulation_hz 60\n"
                  "graphics_tier medium\n"
                  "cpu_budget_mib 4096\n"
                  "gpu_budget_mib 2048\n"
                  "shared_budget_mib 0\n"
                  "loading_budget_mib 512\n"
                  "transition_timeout_seconds 30\n"
                  "acceptance_scenario \"Tests/run.json\"\n";

std::string v2(std::string extra = "") {
    std::string text = kV1;
    text.replace(text.find("shockwave_release 1"), 19, "shockwave_release 2");
    text += "frame_p95_ms 16.667\nframe_p99_ms 20.8\nmax_hitch_ms 50\nlevel_load_seconds 10\n";
    return text + extra;
}

std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const size_t at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}

void testContract() {
    ReleaseFile file;
    Diagnostic error;
    // Version 1 reads; it simply has no budgets.
    CHECK(parseRelease(kV1, "R", file, error));
    CHECK(!file.settings.hasPerformanceBudgets());
    CHECK(contains(releaseText(file.settings), "shockwave_release 1\n"));
    CHECK(!contains(releaseText(file.settings), "frame_p95_ms"));
    // And a version 1 file can't name a version 2 key.
    CHECK(!parseRelease(std::string(kV1) + "frame_p95_ms 16\n", "R", file, error));
    CHECK(error.code == "ReleaseUnknownKey" && contains(error.message, "shockwave_release 2"));
    // Version 2: every budget, round-tripped exactly.
    CHECK(parseRelease(v2(), "R", file, error));
    CHECK(file.settings.frameP95Ms == 16.667 && file.settings.frameP99Ms == 20.8);
    CHECK(file.settings.maxHitchMs == 50 && file.settings.levelLoadSeconds == 10);
    ReleaseFile again;
    CHECK(parseRelease(releaseText(file.settings), "R", again, error) && again.settings == file.settings);
    CHECK(contains(releaseText(file.settings), "shockwave_release 2\n"));
    // Version 2 requires them.
    CHECK(!parseRelease(replaced(v2(), "max_hitch_ms 50\n", ""), "R", file, error));
    CHECK(error.code == "ReleaseMissingKey" && error.object == "max_hitch_ms");
    // Ranges.
    CHECK(!parseRelease(replaced(v2(), "frame_p95_ms 16.667", "frame_p95_ms nan"), "R", file, error));
    CHECK(error.code == "ReleaseInvalidValue" && contains(error.message, "milliseconds"));
    CHECK(!parseRelease(replaced(v2(), "frame_p95_ms 16.667", "frame_p95_ms 0"), "R", file, error));
    // Order: p95 <= p99 <= worst, and a level change within the transition timeout.
    std::vector<Diagnostic> problems;
    CHECK(parseRelease(replaced(v2(), "frame_p95_ms 16.667", "frame_p95_ms 30"), "R", file, error));
    validateRelease(file.settings, nullptr, "R", &file, problems);
    CHECK(!problems.empty() && problems[0].code == "ReleaseFrameBudget");
    problems.clear();
    CHECK(parseRelease(replaced(v2(), "level_load_seconds 10", "level_load_seconds 45"), "R", file, error));
    validateRelease(file.settings, nullptr, "R", &file, problems);
    CHECK(!problems.empty() && problems[0].code == "ReleaseLoadBudget");
    // Set together or not at all.
    ReleaseSettings partial;
    CHECK(parseRelease(kV1, "R", file, error));
    partial = file.settings;
    partial.frameP95Ms = 16;
    problems.clear();
    validateRelease(partial, nullptr, "R", nullptr, problems);
    CHECK(!problems.empty() && problems[0].code == "ReleaseMissingKey");
    // The suggestion: from target_fps.
    ReleaseSettings s = file.settings;
    s.targetFps = 120;
    s.suggestPerformanceBudgets();
    CHECK(s.frameP95Ms == 8.333 && s.frameP99Ms == 10.417 && s.maxHitchMs == 25 && s.levelLoadSeconds == 10);
    CHECK(std::abs(s.frameBudgetMs() - 8.3333333) < 1e-6);
    // The JSON report says null for a budget a version 1 contract doesn't state.
    ReleaseReport report;
    report.present = report.readable = true;
    report.settings = file.settings;
    CHECK(contains(releaseReportJson(report, "x"), "\"frame_p95_ms\": null"));
}

PerformanceRecord goodRecord() {
    PerformanceRecord r;
    r.machine = "Minimum PC";
    r.recorded = "2026-11-09T00:00:00Z";
    r.os = "Linux 6.8";
    r.cpu = "Some CPU";
    r.cores = 8;
    r.ramMib = 16384;
    r.gpu = "GeForce GTX 1060";
    r.gpuKind = "dedicated";
    r.gpuDriver = "NVIDIA 580.95";
    r.gpuMemoryMib = 6144;
    r.buildConfig = "Release";
    r.buildId = "00112233aabbccdd";
    r.renderer = "vulkan";
    r.tier = "medium";
    r.width = 1920;
    r.height = 1080;
    r.scenario = "Tests/perf/tour.json";
    r.laps = 3;
    r.timing = "present";
    r.play = {3000, 9.5, 14.0, 18.0, 31.0, 10.1};
    r.warmup = {120, 11.0, 30.0, 45.0, 60.0, 14.0};
    r.cpuPeakMib = 1500;
    r.gpuPeakMib = 900;
    r.memoryGrowthMib = 4;
    r.pipelinesInPlay = 0;
    r.levels = {{"training", {1500, 9.0, 13.0, 17.0, 25.0, 9.8}}, {"factory", {1500, 10.0, 14.5, 18.5, 31.0, 10.4}}};
    r.loads = {{"training", 2.5, 1200}, {"factory", 3.1, 1500}};
    return r;
}

void testRecords() {
    const PerformanceRecord r = goodRecord();
    const std::string text = performanceText(r);
    PerformanceRecord read;
    Diagnostic error;
    CHECK(parsePerformance(text, "P", read, error));
    // Numbers keep 6 significant digits, which every value here has.
    CHECK(read == r);
    // Strict: an unknown key, a duplicate, a bad value, a missing key, another version.
    CHECK(!parsePerformance(text + "fps 60\n", "P", read, error) && error.object == "fps" && error.line > 0);
    CHECK(!parsePerformance(text + "cores 4\n", "P", read, error) && contains(error.message, "duplicate"));
    CHECK(!parsePerformance(replaced(text, "cores 8", "cores eight"), "P", read, error));
    CHECK(!parsePerformance(replaced(text, "cache warm", "cache lukewarm"), "P", read, error));
    CHECK(!parsePerformance(replaced(text, "laps 3\n", ""), "P", read, error) && error.object == "laps");
    CHECK(!parsePerformance(replaced(text, "shockwave_performance 1", "shockwave_performance 2"), "P", read, error));
    CHECK(!parsePerformance(replaced(text, "machine \"Minimum PC\"", "machine \"Minimum PC"), "P", read, error));
    CHECK(error.code == "PerformanceUnreadable" && !error.error());   // a warning: the gate goes on without it
    // A name with an apostrophe and spaces, escaped and read back.
    PerformanceRecord quoted = r;
    quoted.machine = "Yianni's \"main\" PC";
    CHECK(parsePerformance(performanceText(quoted), "P", read, error) && read.machine == quoted.machine);
    // File names.
    CHECK(performanceFileName("Yianni's desktop") == "yiannis-desktop.swperf");
    CHECK(performanceFileName("Jetson AGX Orin (MAXN)") == "jetson-agx-orin-maxn.swperf");
    CHECK(performanceFileName("???") == "machine.swperf");
    // Written and read from a project's Release/Performance, with a broken one reported.
    const fs::path root = fs::temp_directory_path() / "shockwave_performance_test";
    fs::remove_all(root);
    Project project;
    project.root = root;
    std::string why;
    CHECK(writePerformance(performanceFolder(project) / "b.swperf", r, why));
    CHECK(writePerformance(performanceFolder(project) / "a.swperf", quoted, why));
    {
        FILE* f = std::fopen((performanceFolder(project) / "c.swperf").string().c_str(), "wb");
        std::fputs("not a record\n", f);
        std::fclose(f);
    }
    const PerformanceRecords records = readPerformanceRecords(project);
    CHECK(records.records.size() == 2 && records.problems.size() == 1);
    CHECK(records.files.size() == 2 && contains(records.files[0], "a.swperf"));
    fs::remove_all(root);
}

ReleaseSettings contract() {
    ReleaseFile file;
    Diagnostic error;
    parseRelease(v2(), "R", file, error);
    return file.settings;
}

void testGate() {
    const ReleaseSettings c = contract();
    const std::string build = "00112233aabbccdd";
    PerformanceRecord r = goodRecord();
    CHECK(performanceProblems(c, r, build).empty());
    std::vector<PerformanceRecord> records{r};
    std::vector<std::string> files{"Release/Performance/minimum-pc.swperf"};
    PerformanceGate gate = performanceGate(c, records, files, build);
    CHECK(gate.verdict == PerformanceVerdict::Passed && gate.record == 0 && contains(gate.reason, "every budget met"));

    // Each budget on its own.
    auto problemsWith = [&](auto change) {
        PerformanceRecord copy = goodRecord();
        change(copy);
        return performanceProblems(c, copy, build);
    };
    CHECK(anyContains(problemsWith([](auto& x) { x.play.p95Ms = 17; }), "frame p95"));
    CHECK(anyContains(problemsWith([](auto& x) { x.play.p99Ms = 21; }), "frame p99"));
    CHECK(anyContains(problemsWith([](auto& x) { x.play.worstMs = 51; }), "max_hitch_ms"));
    CHECK(anyContains(problemsWith([](auto& x) { x.play.p50Ms = 17; }), "frame budget"));
    CHECK(anyContains(problemsWith([](auto& x) { x.loads[1].seconds = 10.5; }), "loading factory took 10.5 s"));
    CHECK(anyContains(problemsWith([](auto& x) { x.cpuPeakMib = 5000; }), "cpu_budget_mib"));
    CHECK(anyContains(problemsWith([](auto& x) { x.gpuPeakMib = 3000; }), "gpu_budget_mib"));
    CHECK(anyContains(problemsWith([](auto& x) { x.memoryGrowthMib = 300; }), "isn't freed"));
    CHECK(problemsWith([](auto& x) { x.memoryGrowthMib = 200; }).empty());   // 5% of 4096 = 204
    CHECK(anyContains(problemsWith([](auto& x) { x.pipelinesInPlay = 3; }), "during play"));
    CHECK(problemsWith([](auto& x) { x.pipelinesInPlay = -1; }).empty());   // unknown isn't a failure
    CHECK(anyContains(problemsWith([](auto& x) { x.tier = "low"; }), "promises medium"));
    CHECK(anyContains(problemsWith([](auto& x) { x.tier = "custom"; }), "promises medium"));
    CHECK(problemsWith([](auto& x) { x.tier = "high"; }).empty());   // a higher tier meets a lower promise
    CHECK(anyContains(problemsWith([](auto& x) { x.width = 1024, x.height = 576; }), "under 1280x720"));
    CHECK(anyContains(problemsWith([](auto& x) { x.frameLimit = 30; }), "under the target"));
    CHECK(anyContains(problemsWith([](auto& x) { x.play.frames = 0; }), "no frames"));
    ReleaseSettings shared = c;
    shared.sharedBudgetMib = 1800;
    shared.cpuBudgetMib = 1024;   // each budget met on its own; together over the pool
    shared.gpuBudgetMib = 900;
    PerformanceRecord small = goodRecord();
    small.cpuPeakMib = 1000;
    CHECK(anyContains(performanceProblems(shared, small, build), "shared_budget_mib"));

    // What never counts: no real GPU, a debug build, an older build.
    PerformanceRecord software = goodRecord();
    software.gpu = "llvmpipe";
    software.gpuKind = "software";
    CHECK(anyContains(performanceProblems(c, software, build), "only real hardware counts"));
    PerformanceRecord null = goodRecord();
    null.renderer = "null";
    CHECK(anyContains(performanceProblems(c, null, build), "draws nothing"));
    PerformanceRecord debug = goodRecord();
    debug.buildConfig = "Debug";
    CHECK(anyContains(performanceProblems(c, debug, build), "Debug build"));
    PerformanceRecord stale = goodRecord();
    stale.buildId = "ffffffffffffffff";
    CHECK(anyContains(performanceProblems(c, stale, build), "the game is now 00112233aabbccdd"));
    CHECK(performanceProblems(c, stale, "").empty());   // the current build unknown: nothing to compare

    // The gate: unusable records leave it unverified, a usable one that misses fails it,
    // and any passing record passes it.
    records = {software, stale};
    files = {"a.swperf", "b.swperf"};
    gate = performanceGate(c, records, files, build);
    CHECK(gate.verdict == PerformanceVerdict::Unverified && contains(gate.reason, "no record counts yet"));
    PerformanceRecord slow = goodRecord();
    slow.play.p95Ms = 25;
    slow.play.p99Ms = 26;
    records = {software, slow};
    gate = performanceGate(c, records, files, build);
    CHECK(gate.verdict == PerformanceVerdict::Failed && gate.record == 1 && contains(gate.reason, "b.swperf"));
    records = {software, slow, goodRecord()};
    files = {"a.swperf", "b.swperf", "c.swperf"};
    gate = performanceGate(c, records, files, build);
    CHECK(gate.verdict == PerformanceVerdict::Passed && gate.record == 2);
    gate = performanceGate(c, {}, {}, build);
    CHECK(gate.verdict == PerformanceVerdict::Unverified && contains(gate.reason, "--perf-record"));
    // A contract without budgets fails, whatever the records.
    ReleaseFile v1;
    Diagnostic error;
    parseRelease(kV1, "R", v1, error);
    records = {goodRecord()};
    gate = performanceGate(v1.settings, records, files, build);
    CHECK(gate.verdict == PerformanceVerdict::Failed && contains(gate.reason, "no performance budgets"));

    // Through the release gates.
    ReleaseReport report;
    report.present = report.readable = true;
    report.settings = c;
    ReleaseInputs inputs;
    inputs.performanceChecked = true;
    inputs.performanceRecords = {goodRecord()};
    inputs.performanceFiles = {"Release/Performance/minimum-pc.swperf"};
    inputs.currentBuildId = build;
    releaseGates(report, inputs);
    bool found = false;
    for (const ReleaseGate& g : report.gates)
        if (g.id == "performance") found = g.status == GateStatus::Passed && g.batch == "S10";
    CHECK(found);
    inputs.performanceChecked = false;   // not read: stays unverified, as before S10
    releaseGates(report, inputs);
    for (const ReleaseGate& g : report.gates)
        if (g.id == "performance") CHECK(g.status == GateStatus::Unverified);
}

// Ticks and render interpolation at 30, 60, 144 and uncapped (1000) frames a second, for
// 30, 60 and 120 Hz simulation: the game advances at real time, each frame runs only the
// ticks it owes, and the time drawn (ticks + alpha) never goes back and never drifts more
// than a tick. Then a 5 s load: never thousands of catch-up ticks afterwards.
void testClock() {
    for (const double fps : {30.0, 60.0, 144.0, 1000.0})
        for (const double hz : {30.0, 60.0, 120.0}) {
            SimulationClock clock;
            const double frame = 1.0 / fps, dt = 1.0 / hz;
            uint64_t ticks = 0;
            double real = 0, drawnBefore = -1;
            bool steady = true;
            for (int i = 0; i < int(fps * 10); ++i) {
                const SimulationStep step = clock.advance(frame, frame, dt);
                real += frame;
                ticks += uint64_t(step.ticks);
                steady = steady && step.ticks <= int(std::ceil(frame / dt)) + 1;
                const double drawn = (double(ticks) + clock.alpha(dt)) * dt;
                steady = steady && drawn >= drawnBefore - 1e-9 && std::abs(drawn - real) <= dt + 1e-6;
                drawnBefore = drawn;
            }
            CHECK(steady);
            CHECK(std::llabs(int64_t(ticks) - int64_t(std::llround(hz * 10))) <= 1);
        }
    // Offline (smoothed): a 5 s stall is at most 0.25 s of game time, 15 ticks at 60 Hz.
    SimulationClock offline;
    const SimulationStep afterLoad = offline.advance(5.0, 5.0, 1.0 / 60);
    CHECK(afterLoad.ticks <= 16 && afterLoad.frameTime <= SimulationClock::kMaxFrameTime);
    // Online (monotonic): at most kMaxCatchUpTicks, and the debt is dropped as an overrun.
    SimulationClock online;
    online.setSource(SimulationTime::Monotonic);
    const SimulationStep stalled = online.advance(5.0, 5.0, 1.0 / 60);
    CHECK(stalled.ticks <= SimulationClock::kMaxCatchUpTicks && stalled.overrun && stalled.debt < 5.0);
    CHECK(online.advance(1.0 / 60, 1.0 / 60, 1.0 / 60).ticks <= 2);
}

}   // namespace

int main() {
    testClock();
    testPercentiles();
    testContract();
    testRecords();
    testGate();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("performance: all checks passed\n");
    return 0;
}
