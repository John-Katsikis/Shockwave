#pragma once

// Performance records (S10, docs/Performance.md): what a measured run on one named machine
// showed, kept as evidence for the release's performance gate.
//
// `player --perf-run <scenario> --perf-record "<machine>"` plays a flow-test scenario in real
// time and writes Release/Performance/<machine>.swperf:
//
//   shockwave_performance 1
//   machine "Yianni's desktop"
//   recorded "2026-11-09T01:30:00Z"
//   os "Linux 6.8.0-45-generic"
//   cpu "AMD Ryzen 7 5800X"
//   ...
//   p95_ms 14.2
//   level "factory" 1800 9.1 14.6 18.3 31.0
//   load "factory" 2.41 1380
//
// The gate reads every record and passes when one of them, from an optimised build on real
// hardware at the contract's tier, meets every budget. A software or null renderer, a debug
// build or a record from an older build never counts. Nothing is ticked by hand.
//
// Records are text, one key per line, `#` starts a comment. Unknown keys are errors with
// file:line, so a typo can't quietly drop a number. Reading is strict and changes nothing
// on failure; writing goes through a temporary file.

#include <Projects/Diagnostic.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

struct Project;
struct ReleaseSettings;

// Frame times over one stretch of play (a level between loads, or the whole run).
struct PerformanceFrames {
    uint64_t frames = 0;
    double p50Ms = 0, p95Ms = 0, p99Ms = 0;
    double worstMs = 0;   // the slowest frame: the worst hitch a player saw
    double averageMs = 0;
    bool operator==(const PerformanceFrames&) const = default;
};

// One level as the run played it: every visit's frames together.
struct PerformanceLevel {
    std::string id;
    PerformanceFrames frames;
    bool operator==(const PerformanceLevel&) const = default;
};

// One level change: how long it took and the most memory held while it ran.
struct PerformanceLoad {
    std::string level;   // where it went
    double seconds = 0;
    uint64_t peakMib = 0;   // process memory at its highest during the load
    bool operator==(const PerformanceLoad&) const = default;
};

struct PerformanceRecord {
    // The machine. "machine" is the name people know it by; the rest is what it reported.
    std::string machine;
    std::string recorded;   // UTC, ISO 8601
    std::string os;   // "Linux 6.8.0-45-generic"
    std::string cpu;
    uint32_t cores = 0;
    uint64_t ramMib = 0;
    std::string gpu;
    std::string gpuKind;   // "dedicated", "integrated", "virtual", "software", "other", "none"
    std::string gpuDriver;
    uint64_t gpuMemoryMib = 0;
    bool unifiedMemory = false;   // the CPU and GPU share one pool (Orin, Apple, most laptops' iGPUs)
    // The build and how it ran.
    std::string buildConfig;   // "Release", "Debug", ... ("" unknown)
    std::string buildId;   // the cook's build_id, "" without a cook
    std::string renderer;   // "vulkan", "null"
    std::string tier;   // the graphics preset the settings match, or "custom"
    uint32_t width = 0, height = 0;   // the size drawn at, after render scale
    double renderScale = 1;
    bool vsync = false;
    double frameLimit = 0;   // 0 none
    bool coldCache = false;   // the pipeline and texture caches were emptied first
    std::string scenario;   // the flow-test scenario, project-relative
    uint32_t laps = 1;
    std::string timing;   // "present" (frames reaching the screen) or "frame" (CPU frame starts)
    // What it showed. Play only: loading is in `loads`, never mixed in.
    PerformanceFrames play;
    double warmupSeconds = 0;   // the first second of each level, reported apart (not deleted)
    PerformanceFrames warmup;
    uint64_t cpuPeakMib = 0;   // process memory at its highest over the run
    uint64_t gpuPeakMib = 0;   // GPU memory the renderer held at its highest
    // How much memory grew from the end of the first lap to the end of the last; with one
    // lap, 0. A level rotation that keeps growing leaks.
    int64_t memoryGrowthMib = 0;
    int64_t pipelinesInPlay = -1;   // pipelines built during play rather than loading; -1 unknown
    std::vector<PerformanceLevel> levels;
    std::vector<PerformanceLoad> loads;

    bool operator==(const PerformanceRecord&) const = default;
};

// ---- Files ------------------------------------------------------------------------------

// Release/Performance under the project, where records live.
std::filesystem::path performanceFolder(const Project& project);
// A file name for a machine: lowercase letters, digits and '-' ("Yianni's desktop" ->
// "yiannis-desktop.swperf"); "machine.swperf" when nothing is left.
std::string performanceFileName(std::string_view machine);

std::string performanceText(const PerformanceRecord& record);
// The first problem stops it, into `error` with `shown` and the line. False leaves `out`.
bool parsePerformance(std::string_view text, std::string_view shown, PerformanceRecord& out, Diagnostic& error);
bool readPerformance(
    const std::filesystem::path& file, std::string_view shown, PerformanceRecord& out, Diagnostic& error);
bool writePerformance(const std::filesystem::path& file, const PerformanceRecord& record, std::string& error);

struct PerformanceRecords {
    std::vector<PerformanceRecord> records;
    std::vector<std::string> files;   // project-relative, one per record
    std::vector<Diagnostic> problems;   // records that don't read
};
// Every .swperf in the project's Release/Performance, sorted by file name.
PerformanceRecords readPerformanceRecords(const Project& project);

// ---- The gate ---------------------------------------------------------------------------

// The memory a lap may grow by before it counts as a leak: 5% of the CPU budget, at
// least 32 MiB.
uint64_t memoryGrowthAllowanceMib(const ReleaseSettings& contract);

// Why one record does or doesn't meet the contract: every problem, in a stable order.
// Empty when it qualifies. `currentBuildId` is the game's build now ("" unknown): a record
// from another build is stale.
std::vector<std::string>
performanceProblems(const ReleaseSettings& contract, const PerformanceRecord& record, std::string_view currentBuildId);

enum class PerformanceVerdict { Passed, Failed, Unverified };
struct PerformanceGate {
    PerformanceVerdict verdict = PerformanceVerdict::Unverified;
    std::string reason;
    int record = -1;   // the record it rests on (the passing one, or the closest), or -1
};
// Passed when any record qualifies. Failed when the contract has no budgets, or a record
// from real hardware on this build misses one. Unverified when there's no usable record:
// none yet, only software renderers or stale builds.
PerformanceGate performanceGate(
    const ReleaseSettings& contract, std::span<const PerformanceRecord> records, std::span<const std::string> files,
    std::string_view currentBuildId);

}   // namespace shockwave
