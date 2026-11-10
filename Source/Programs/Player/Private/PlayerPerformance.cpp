// --perf-run: a flow-test scenario played in real time and measured (S10,
// docs/Performance.md). Loading, the first second of each level and play are kept apart;
// nothing is deleted. The report says where the time went and what memory did, and
// --perf-record keeps it as Release/Performance/<machine>.swperf for the release's
// performance gate.
#include "Player.h"

#include <Core/FramePacing.h>
#include <Engine/Engine.h>
#include <Engine/GraphicsSettings.h>
#include <Packaging/Package.h>
#include <Platform/SystemInfo.h>
#include <Projects/Performance.h>
#include <Projects/ReleaseSettings.h>
#include <Renderer/GpuSelection.h>
#include <Renderer/Renderer.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>

namespace player {

namespace {

constexpr double kWarmupSeconds = 1.0;   // after each arrival: first uploads, caches, clocks ramping up
constexpr int kMemoryEvery = 10;   // frames between memory reads during play (every frame while loading)

uint64_t mib(uint64_t bytes) { return (bytes + (1u << 20) - 1) >> 20; }

shockwave::PerformanceFrames framesOf(const std::vector<shockwave::FrameSample>& samples) {
    const shockwave::FrameSummary s = shockwave::summarizeFrames(samples);
    shockwave::PerformanceFrames f;
    f.frames = s.frames;
    f.p50Ms = s.p50Ms;
    f.p95Ms = s.p95Ms;
    f.p99Ms = s.p99Ms;
    f.worstMs = s.maxMs;
    f.averageMs = s.averageMs;
    return f;
}

std::string utcNow() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

std::string jsonString(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\u%04x", unsigned(static_cast<unsigned char>(c)));
            out += buffer;
            continue;
        }
        out += c;
    }
    return out + "\"";
}

std::string framesJson(const shockwave::PerformanceFrames& f) {
    char text[256];
    std::snprintf(
        text, sizeof text,
        "{\"frames\": %llu, \"p50_ms\": %.4f, \"p95_ms\": %.4f, \"p99_ms\": %.4f, \"worst_ms\": %.4f, "
        "\"average_ms\": %.4f}",
        static_cast<unsigned long long>(f.frames), f.p50Ms, f.p95Ms, f.p99Ms, f.worstMs, f.averageMs);
    return text;
}

std::string framesLine(const shockwave::PerformanceFrames& f) {
    char text[200];
    std::snprintf(
        text, sizeof text, "%llu frames, p50 %.2f ms, p95 %.2f ms, p99 %.2f ms, worst %.2f ms",
        static_cast<unsigned long long>(f.frames), f.p50Ms, f.p95Ms, f.p99Ms, f.worstMs);
    return text;
}

}   // namespace

struct Player::PerfState {
    size_t lapSteps = 0;
    enum class Phase { Loading, Warmup, Play } phase = Phase::Warmup;
    double phaseStart = 0;   // engine seconds the current warm-up began
    uint64_t epoch = 0;   // the level epoch last seen
    uint64_t lastResult = 0;   // the newest travel result filed
    std::vector<shockwave::FrameSample> play, warmup, loading;
    std::map<std::string, std::vector<shockwave::FrameSample>> levels;
    std::vector<std::string> levelOrder;   // first visit first
    std::vector<shockwave::PerformanceLoad> loads;
    uint64_t loadPeakBytes = 0;   // the most resident memory seen since the current load began
    uint64_t cpuPeakBytes = 0, gpuPeakBytes = 0;
    std::vector<uint64_t> lapEndBytes;   // resident memory as each lap finished
    size_t lapsDone = 0;
    uint64_t frame = 0;
    std::string timing;   // "present" while every frame had a present interval, else "frame"
    bool presentTimed = true;
    // Pipelines (S10): the renderer's count at the last sample, and how many were made
    // off the loading screen (in warm-up or play frames).
    uint64_t pipelinesSeen = 0;
    int64_t pipelinesInPlay = 0;
};

Player::Player(shockwave::Project project, Tool tool, const shockwave::GameModule& module)
    : m_project(std::move(project)), m_tool(std::move(tool)), m_module(module) {}
Player::~Player() = default;

void Player::startPerformance(size_t lapSteps) {
    m_perf = std::make_unique<PerfState>();
    m_perf->lapSteps = std::max<size_t>(lapSteps, 1);
    m_perf->phaseStart = engine().frameStartSeconds();
    m_perf->epoch = m_levels ? m_levels->epoch() : 0;
    m_perf->pipelinesSeen = engine().renderer().pipelinesCreated();
    // A fresh window on the frames: the run starts with this level's warm-up.
    engine().frameStats().clear();
    SW_LOG(
        shockwave::LogGame, Info, "Performance run: {} lap{}, {} cache", m_tool.perfLaps,
        m_tool.perfLaps == 1 ? "" : "s", m_tool.coldCache ? "cold" : "warm");
}

void Player::samplePerformance() {
    PerfState& p = *m_perf;
    const double now = engine().frameStartSeconds();
    ++p.frame;

    // Memory: every frame while loading (that's where the peaks are), every few otherwise.
    const bool loading = !m_levels || m_levels->transitioning() || !m_levels->playing();
    if (loading || p.frame % kMemoryEvery == 0) {
        const shockwave::ProcessMemory memory = shockwave::processMemory();
        p.cpuPeakBytes = std::max({p.cpuPeakBytes, memory.residentBytes, memory.peakResidentBytes});
        p.loadPeakBytes = std::max(p.loadPeakBytes, memory.residentBytes);
        p.gpuPeakBytes = std::max(p.gpuPeakBytes, engine().renderer().memoryReport().gpuBytes());
    }

    // Travel that ended since the last frame: one load each, with its time from accepted to
    // ended. Instant travel never shows a transitioning frame, so the epoch says it too.
    bool arrived = false;
    if (m_levels) {
        for (const shockwave::TravelResult& result : m_levels->results()) {
            if (result.id <= p.lastResult) continue;
            p.lastResult = result.id;
            if (result.status != shockwave::TravelStatus::Succeeded) continue;
            shockwave::PerformanceLoad load;
            load.level = result.request.level;
            if (const auto status = m_levels->status(result.id)) load.seconds = status->durationMs / 1000.0;
            load.peakMib = mib(std::max(p.loadPeakBytes, shockwave::processMemory().residentBytes));
            p.loads.push_back(std::move(load));
            arrived = true;
        }
        if (m_levels->epoch() != p.epoch) {
            p.epoch = m_levels->epoch();
            arrived = true;
        }
    }

    // The last frame's sample, under what this frame was doing. It's a frame or two old,
    // which the warm-up after each arrival covers: a load's long frame lands in loading or
    // warm-up, never in play.
    const shockwave::FrameSample sample = engine().frameStats().latest();
    if (sample.frameMs > 0) {
        if (sample.presentMs <= 0) p.presentTimed = false;
        switch (p.phase) {
        case PerfState::Phase::Loading: p.loading.push_back(sample); break;
        case PerfState::Phase::Warmup: p.warmup.push_back(sample); break;
        case PerfState::Phase::Play: {
            p.play.push_back(sample);
            const std::string& level = m_levels ? m_levels->current().level : std::string();
            auto& list = p.levels[level];
            if (list.empty()) p.levelOrder.push_back(level);
            list.push_back(sample);
            break;
        }
        }
    }

    // Pipelines made since the last sample belong to the frame that just ran: off the
    // loading screen, each one was a stall the player could feel.
    if (const uint64_t made = engine().renderer().pipelinesCreated(); made != p.pipelinesSeen) {
        if (p.phase != PerfState::Phase::Loading) p.pipelinesInPlay += int64_t(made - p.pipelinesSeen);
        p.pipelinesSeen = made;
    }

    // What this frame is, for the next sample.
    if (loading) {
        if (p.phase != PerfState::Phase::Loading) p.loadPeakBytes = 0;
        p.phase = PerfState::Phase::Loading;
    } else if (arrived || p.phase == PerfState::Phase::Loading) {
        p.phase = PerfState::Phase::Warmup;
        p.phaseStart = now;
        p.loadPeakBytes = 0;
    } else if (p.phase == PerfState::Phase::Warmup && now - p.phaseStart >= kWarmupSeconds) {
        p.phase = PerfState::Phase::Play;
    }

    // Laps: memory as each one ends, to see whether a rotation keeps growing.
    if (m_flowTest && m_tool.perfLaps > 1) {
        const size_t lapsDone = std::min<size_t>(m_flowTest->step() / p.lapSteps, m_tool.perfLaps);
        while (p.lapsDone < lapsDone) {
            ++p.lapsDone;
            p.lapEndBytes.push_back(shockwave::processMemory().residentBytes);
        }
    }
}

void Player::endPerformance(bool passed) {
    PerfState& p = *m_perf;
    shockwave::Renderer& renderer = engine().renderer();
    const shockwave::SystemInfo& system = shockwave::systemInfo();

    shockwave::PerformanceRecord r;
    r.machine = m_tool.perfRecord;
    r.recorded = utcNow();
    r.os = system.os + (system.osVersion.empty() ? "" : " " + system.osVersion);
    r.cpu = system.cpu;
    r.cores = system.logicalCores;
    r.ramMib = system.ramMib;
    r.renderer = renderer.backendName();
    r.gpu = renderer.gpuName() ? renderer.gpuName() : "";
    r.gpuKind = "none";
    if (const shockwave::GpuChoice* choice = renderer.gpuChoice())
        if (const shockwave::GpuCandidate* gpu = choice->chosenGpu()) {
            switch (gpu->kind) {   // one word each: the record's gpu_kind
            case shockwave::GpuKind::Discrete: r.gpuKind = "dedicated"; break;
            case shockwave::GpuKind::Integrated: r.gpuKind = "integrated"; break;
            case shockwave::GpuKind::Virtual: r.gpuKind = "virtual"; break;
            case shockwave::GpuKind::Software: r.gpuKind = "software"; break;
            case shockwave::GpuKind::Other: r.gpuKind = "other"; break;
            }
            r.gpuDriver = gpu->driver;
            r.gpuMemoryMib = gpu->memoryMB;
            r.unifiedMemory = gpu->kind == shockwave::GpuKind::Integrated;
        }
    r.buildConfig = shockwave::engineBuildConfig();
    r.buildId = currentBuildId(m_project);
    const shockwave::GraphicsSettings graphics = engine().graphics().current();
    const auto preset = shockwave::matchingGraphicsPreset(graphics);
    r.tier = preset ? shockwave::graphicsPresetName(*preset) : "custom";
    r.width = renderer.renderWidth();
    r.height = renderer.renderHeight();
    r.renderScale = graphics.renderScale;
    r.vsync = graphics.vsync;
    r.frameLimit = engine().frameLimit();
    r.coldCache = m_tool.coldCache;
    r.scenario = m_project.relative(fs::absolute(m_tool.flowTest));
    r.laps = m_tool.perfLaps;
    r.timing = p.presentTimed && !p.play.empty() ? "present" : "frame";
    r.play = framesOf(p.play);
    r.warmupSeconds = kWarmupSeconds;
    r.warmup = framesOf(p.warmup);
    r.cpuPeakMib = mib(p.cpuPeakBytes);
    r.gpuPeakMib = mib(p.gpuPeakBytes);
    if (p.lapEndBytes.size() >= 2)
        r.memoryGrowthMib = int64_t(p.lapEndBytes.back() >> 20) - int64_t(p.lapEndBytes.front() >> 20);
    // The null renderer has no pipelines: unknown, not zero.
    r.pipelinesInPlay = p.pipelinesSeen ? p.pipelinesInPlay : -1;
    for (const std::string& id : p.levelOrder) r.levels.push_back({id, framesOf(p.levels[id])});
    r.loads = p.loads;
    const shockwave::PerformanceFrames loadingFrames = framesOf(p.loading);

    // For people.
    std::printf(
        "perf run: %s, %u lap%s, %s cache\n", r.scenario.c_str(), r.laps, r.laps == 1 ? "" : "s",
        r.coldCache ? "cold" : "warm");
    std::printf(
        "machine:  %s; %s; %s (%u threads, %llu MiB)\n", r.machine.empty() ? "(unnamed)" : r.machine.c_str(),
        r.os.c_str(), r.cpu.empty() ? "unknown CPU" : r.cpu.c_str(), r.cores,
        static_cast<unsigned long long>(r.ramMib));
    std::printf(
        "gpu:      %s (%s, %s), %s renderer\n", r.gpu.empty() ? "none" : r.gpu.c_str(), r.gpuKind.c_str(),
        r.gpuDriver.empty() ? "no driver" : r.gpuDriver.c_str(), r.renderer.c_str());
    std::printf(
        "build:    %s%s\n", r.buildConfig.empty() ? "unknown" : r.buildConfig.c_str(),
        r.buildId.empty() ? ", not cooked" : (", build " + r.buildId).c_str());
    std::printf(
        "drawn:    %ux%u at %s (render scale %.2f), vsync %s, frame limit %s\n", r.width, r.height, r.tier.c_str(),
        r.renderScale, r.vsync ? "on" : "off",
        r.frameLimit > 0 ? (std::to_string(int(r.frameLimit)) + " fps").c_str() : "none");
    std::printf(
        "timing:   %s\n", r.timing == "present"
                              ? "intervals between frames reaching the screen"
                              : "intervals between frame starts (the driver doesn't report presents)");
    std::printf("play:     %s\n", framesLine(r.play).c_str());
    for (const shockwave::PerformanceLevel& level : r.levels)
        std::printf("  level %-16s %s\n", level.id.c_str(), framesLine(level.frames).c_str());
    std::printf("warm-up:  %s (the first %.0f s of each level, apart)\n", framesLine(r.warmup).c_str(), kWarmupSeconds);
    std::printf("loading:  %zu changes, %s on the loading screen\n", r.loads.size(), framesLine(loadingFrames).c_str());
    for (const shockwave::PerformanceLoad& load : r.loads)
        std::printf(
            "  load %-17s %.2f s, peak %llu MiB\n", load.level.c_str(), load.seconds,
            static_cast<unsigned long long>(load.peakMib));
    std::printf(
        "memory:   peak %llu MiB process, %llu MiB GPU", static_cast<unsigned long long>(r.cpuPeakMib),
        static_cast<unsigned long long>(r.gpuPeakMib));
    if (r.laps > 1)
        std::printf(", %+lld MiB from the first lap's end to the last", static_cast<long long>(r.memoryGrowthMib));
    std::printf("\n");
    if (r.pipelinesInPlay >= 0)
        std::printf(
            "pipelines: %lld built during play, %llu by warm-up while loading (%s)\n",
            static_cast<long long>(r.pipelinesInPlay),
            static_cast<unsigned long long>(engine().renderer().warmedPipelines()),
            engine().renderer().pipelineWarmup() ? "on" : "off");

    // Against the contract, when there is one: what the performance gate would say of it.
    const shockwave::ReleaseReport release = shockwave::releaseReport(m_project, releaseInputs(m_project));
    if (release.readable && release.settings.hasPerformanceBudgets()) {
        const std::vector<std::string> problems = shockwave::performanceProblems(release.settings, r, r.buildId);
        if (problems.empty()) std::printf("budgets:  every budget in %s met\n", release.file.c_str());
        for (const std::string& problem : problems) std::printf("budget:   %s\n", problem.c_str());
    } else if (release.readable) {
        std::printf("budgets:  %s states none yet (docs/Performance.md)\n", release.file.c_str());
    }

    if (!m_tool.benchmarkJson.empty()) {
        std::string json = "{\n  \"format\": 1,\n  \"mode\": \"perf-run\",\n";
        json += "  \"passed\": " + std::string(passed ? "true" : "false") + ",\n";
        json += "  \"scenario\": " + jsonString(r.scenario) + ",\n";
        json += "  \"laps\": " + std::to_string(r.laps) + ",\n";
        json += "  \"cache\": \"" + std::string(r.coldCache ? "cold" : "warm") + "\",\n";
        json += "  \"machine\": {\"name\": " + jsonString(r.machine) + ", \"os\": " + jsonString(r.os) +
                ", \"cpu\": " + jsonString(r.cpu) + ", \"cores\": " + std::to_string(r.cores) +
                ", \"ram_mib\": " + std::to_string(r.ramMib) + "},\n";
        json += "  \"gpu\": {\"name\": " + jsonString(r.gpu) + ", \"kind\": " + jsonString(r.gpuKind) +
                ", \"driver\": " + jsonString(r.gpuDriver) + ", \"memory_mib\": " + std::to_string(r.gpuMemoryMib) +
                ", \"unified_memory\": " + (r.unifiedMemory ? "true" : "false") + "},\n";
        json += "  \"renderer\": " + jsonString(r.renderer) + ",\n";
        json += "  \"build\": {\"config\": " + jsonString(r.buildConfig) + ", \"build_id\": " + jsonString(r.buildId) +
                "},\n";
        json += "  \"tier\": " + jsonString(r.tier) + ",\n";
        json += "  \"resolution\": [" + std::to_string(r.width) + ", " + std::to_string(r.height) + "],\n";
        json += "  \"timing\": " + jsonString(r.timing) + ",\n";
        json += "  \"play\": " + framesJson(r.play) + ",\n";
        json += "  \"warmup\": " + framesJson(r.warmup) + ",\n";
        json += "  \"loading\": " + framesJson(loadingFrames) + ",\n";
        json += "  \"levels\": [";
        for (size_t i = 0; i < r.levels.size(); ++i)
            json += std::string(i ? ", " : "") + "{\"id\": " + jsonString(r.levels[i].id) +
                    ", \"frames\": " + framesJson(r.levels[i].frames) + "}";
        json += "],\n  \"loads\": [";
        for (size_t i = 0; i < r.loads.size(); ++i) {
            char text[160];
            std::snprintf(
                text, sizeof text, "\"seconds\": %.4f, \"peak_mib\": %llu}", r.loads[i].seconds,
                static_cast<unsigned long long>(r.loads[i].peakMib));
            json += std::string(i ? ", " : "") + "{\"level\": " + jsonString(r.loads[i].level) + ", " + text;
        }
        json += "],\n";
        json += "  \"memory\": {\"cpu_peak_mib\": " + std::to_string(r.cpuPeakMib) +
                ", \"gpu_peak_mib\": " + std::to_string(r.gpuPeakMib) +
                ", \"growth_mib\": " + std::to_string(r.memoryGrowthMib) + "},\n";
        json += "  \"flow\": " + m_flowTest->reportJson();
        while (!json.empty() && json.back() == '\n') json.pop_back();
        json += "\n}\n";
        if (m_tool.benchmarkJson == "-") {
            std::fputs(json.c_str(), stdout);
        } else {
            std::ofstream out(m_tool.benchmarkJson, std::ios::binary);
            out << json;
            out.close();
            if (out)
                std::printf("Wrote %s\n", m_tool.benchmarkJson.string().c_str());
            else
                std::fprintf(stderr, "Player: can't write %s\n", m_tool.benchmarkJson.string().c_str());
        }
    }

    // A record only of a run that finished: a failed flow measured something else.
    if (!m_tool.perfRecord.empty()) {
        if (!passed) {
            std::fprintf(stderr, "Player: the scenario didn't pass, so no performance record was written\n");
        } else {
            const fs::path folder =
                m_tool.perfRecordFolder.empty() ? shockwave::performanceFolder(m_project) : m_tool.perfRecordFolder;
            const fs::path file = folder / shockwave::performanceFileName(r.machine);
            std::string error;
            if (shockwave::writePerformance(file, r, error)) {
                std::printf("Wrote %s\n", file.string().c_str());
            } else {
                std::fprintf(stderr, "Player: %s\n", error.c_str());
                m_exitCode = 1;
            }
        }
    }
    m_perf.reset();
}

}   // namespace player
