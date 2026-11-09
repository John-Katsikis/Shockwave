// Performance records and the performance gate (see Performance.h, docs/Performance.md).
#include <Projects/Performance.h>

#include <Projects/Project.h>
#include <Projects/ReleaseSettings.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace shockwave {

namespace {

using P = PerformanceRecord;

std::string number(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(6) << value;
    return out.str();
}

std::string quoted(const std::string& text) {
    std::ostringstream out;
    out << std::quoted(text);
    return out.str();
}

bool parseNumber(std::string_view text, double& out) {
    double value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) return false;
    out = value;
    return true;
}

template <class T>
bool parseWhole(std::string_view text, T& out) {
    T value{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) return false;
    out = value;
    return true;
}

// One scalar key: how to write it and how to read its values (already split, quotes
// removed). Lists (level, load) are handled apart.
struct Key {
    const char* name;
    std::function<std::string(const P&)> write;
    std::function<bool(P&, const std::vector<std::string>&)> read;
};

Key text(const char* name, std::string P::*member) {
    return {
        name, [member](const P& r) { return quoted(r.*member); },
        [member](P& r, const std::vector<std::string>& v) {
            if (v.size() != 1) return false;
            r.*member = v[0];
            return true;
        }};
}
Key word(const char* name, std::string P::*member) {
    return {
        name, [member](const P& r) { return (r.*member).empty() ? std::string("\"\"") : r.*member; },
        [member](P& r, const std::vector<std::string>& v) {
            if (v.size() != 1) return false;
            r.*member = v[0];
            return true;
        }};
}
template <class T>
Key whole(const char* name, T P::*member) {
    return {
        name, [member](const P& r) { return std::to_string(r.*member); },
        [member](P& r, const std::vector<std::string>& v) { return v.size() == 1 && parseWhole(v[0], r.*member); }};
}
Key real(const char* name, double P::*member) {
    return {
        name, [member](const P& r) { return number(r.*member); },
        [member](P& r, const std::vector<std::string>& v) {
            return v.size() == 1 && parseNumber(v[0], r.*member) && r.*member >= 0;
        }};
}
Key flag(const char* name, bool P::*member) {
    return {
        name, [member](const P& r) { return std::string(r.*member ? "true" : "false"); },
        [member](P& r, const std::vector<std::string>& v) {
            if (v.size() != 1 || (v[0] != "true" && v[0] != "false")) return false;
            r.*member = v[0] == "true";
            return true;
        }};
}

// frames p50 p95 p99 worst average
std::string framesText(const PerformanceFrames& f) {
    return std::to_string(f.frames) + " " + number(f.p50Ms) + " " + number(f.p95Ms) + " " + number(f.p99Ms) + " " +
           number(f.worstMs) + " " + number(f.averageMs);
}
bool readFrames(std::span<const std::string> v, PerformanceFrames& f) {
    return v.size() == 6 && parseWhole(v[0], f.frames) && parseNumber(v[1], f.p50Ms) && parseNumber(v[2], f.p95Ms) &&
           parseNumber(v[3], f.p99Ms) && parseNumber(v[4], f.worstMs) && parseNumber(v[5], f.averageMs);
}
Key frames(const char* name, PerformanceFrames P::*member) {
    return {
        name, [member](const P& r) { return framesText(r.*member); },
        [member](P& r, const std::vector<std::string>& v) { return readFrames(v, r.*member); }};
}

const std::vector<Key>& keys() {
    static const std::vector<Key> table = {
        text("machine", &P::machine),
        text("recorded", &P::recorded),
        text("os", &P::os),
        text("cpu", &P::cpu),
        whole("cores", &P::cores),
        whole("ram_mib", &P::ramMib),
        text("gpu", &P::gpu),
        word("gpu_kind", &P::gpuKind),
        text("gpu_driver", &P::gpuDriver),
        whole("gpu_memory_mib", &P::gpuMemoryMib),
        flag("unified_memory", &P::unifiedMemory),
        word("build_config", &P::buildConfig),
        text("build_id", &P::buildId),
        word("renderer", &P::renderer),
        word("tier", &P::tier),
        {"resolution", [](const P& r) { return std::to_string(r.width) + " " + std::to_string(r.height); },
         [](P& r, const std::vector<std::string>& v) {
             return v.size() == 2 && parseWhole(v[0], r.width) && parseWhole(v[1], r.height);
         }},
        real("render_scale", &P::renderScale),
        flag("vsync", &P::vsync),
        real("frame_limit", &P::frameLimit),
        {"cache", [](const P& r) { return std::string(r.coldCache ? "cold" : "warm"); },
         [](P& r, const std::vector<std::string>& v) {
             if (v.size() != 1 || (v[0] != "cold" && v[0] != "warm")) return false;
             r.coldCache = v[0] == "cold";
             return true;
         }},
        text("scenario", &P::scenario),
        whole("laps", &P::laps),
        word("timing", &P::timing),
        frames("play", &P::play),
        real("warmup_seconds", &P::warmupSeconds),
        frames("warmup", &P::warmup),
        whole("cpu_peak_mib", &P::cpuPeakMib),
        whole("gpu_peak_mib", &P::gpuPeakMib),
        whole("memory_growth_mib", &P::memoryGrowthMib),
        whole("pipelines_in_play", &P::pipelinesInPlay),
    };
    return table;
}

// Splits a line into words, keeping "quoted text" (with \" and \\) as one. Stops at an
// unquoted #. False on an unclosed quote.
bool splitLine(const std::string& line, std::vector<std::string>& words) {
    words.clear();
    size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }
        if (c == '#') break;
        std::string word;
        if (c == '"') {
            ++i;
            bool closed = false;
            while (i < line.size()) {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    word += line[i + 1];
                    i += 2;
                } else if (line[i] == '"') {
                    closed = true;
                    ++i;
                    break;
                } else {
                    word += line[i++];
                }
            }
            if (!closed) return false;
        } else {
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') word += line[i++];
        }
        words.push_back(std::move(word));
    }
    return true;
}

}   // namespace

std::filesystem::path performanceFolder(const Project& project) { return project.root / "Release" / "Performance"; }

std::string performanceFileName(std::string_view machine) {
    std::string out;
    for (const char raw : machine) {
        const char c = (raw >= 'A' && raw <= 'Z') ? char(raw - 'A' + 'a') : raw;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            out += c;
        else if ((c == ' ' || c == '-' || c == '_' || c == '.') && !out.empty() && out.back() != '-')
            out += '-';
        // Anything else (an apostrophe, a non-ASCII letter) is dropped.
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.size() > 64) out.resize(64);
    return (out.empty() ? std::string("machine") : out) + ".swperf";
}

std::string performanceText(const PerformanceRecord& record) {
    std::string out = "# A measured run (docs/Performance.md), written by player --perf-run --perf-record.\n"
                      "shockwave_performance 1\n";
    for (const Key& key : keys()) out += std::string(key.name) + " " + key.write(record) + "\n";
    for (const PerformanceLevel& level : record.levels)
        out += "level " + quoted(level.id) + " " + framesText(level.frames) + "\n";
    for (const PerformanceLoad& load : record.loads)
        out += "load " + quoted(load.level) + " " + number(load.seconds) + " " + std::to_string(load.peakMib) + "\n";
    return out;
}

bool parsePerformance(std::string_view text, std::string_view shown, PerformanceRecord& out, Diagnostic& error) {
    PerformanceRecord result;
    std::istringstream in{std::string(text)};
    std::string line;
    std::vector<std::string> words;
    std::vector<bool> seen(keys().size(), false);
    int lineNumber = 0;
    bool header = false;
    auto fail = [&](const std::string& object, const std::string& message) {
        error = {"PerformanceUnreadable", DiagnosticSeverity::Warning, std::string(shown), lineNumber, object, message};
        return false;
    };
    while (std::getline(in, line)) {
        ++lineNumber;
        if (!splitLine(line, words)) return fail("", "a quote isn't closed");
        if (words.empty()) continue;
        const std::string key = words[0];
        const std::vector<std::string> values(words.begin() + 1, words.end());
        if (!header) {
            if (key != "shockwave_performance" || values.size() != 1)
                return fail("", "expected shockwave_performance 1 as the first line");
            if (values[0] != "1")
                return fail("", "unsupported performance record version " + values[0] + " (this build reads 1)");
            header = true;
            continue;
        }
        if (key == "level") {
            PerformanceLevel level;
            if (values.size() != 7 || !readFrames(std::span(values).subspan(1), level.frames))
                return fail(key, "level takes \"id\" frames p50 p95 p99 worst average");
            level.id = values[0];
            result.levels.push_back(std::move(level));
            continue;
        }
        if (key == "load") {
            PerformanceLoad load;
            if (values.size() != 3 || !parseNumber(values[1], load.seconds) || !parseWhole(values[2], load.peakMib))
                return fail(key, "load takes \"level\" seconds peak_mib");
            load.level = values[0];
            result.loads.push_back(std::move(load));
            continue;
        }
        const auto& table = keys();
        const auto found = std::find_if(table.begin(), table.end(), [&](const Key& k) { return key == k.name; });
        if (found == table.end()) return fail(key, "unknown key " + key);
        const size_t index = size_t(found - table.begin());
        if (seen[index]) return fail(key, "duplicate key " + key);
        if (!found->read(result, values)) return fail(key, "bad value for " + key);
        seen[index] = true;
    }
    if (!header) return fail("", "expected shockwave_performance 1 as the first line");
    for (size_t i = 0; i < seen.size(); ++i)
        if (!seen[i]) {
            lineNumber = 0;
            return fail(keys()[i].name, std::string("missing ") + keys()[i].name);
        }
    out = std::move(result);
    return true;
}

bool readPerformance(
    const std::filesystem::path& file, std::string_view shown, PerformanceRecord& out, Diagnostic& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = {"PerformanceUnreadable", DiagnosticSeverity::Warning, std::string(shown), 0, "", "can't open it"};
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parsePerformance(text.str(), shown, out, error);
}

bool writePerformance(const std::filesystem::path& file, const PerformanceRecord& record, std::string& error) {
    std::error_code ec;
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        error = file.string() + ": " + ec.message();
        return false;
    }
    const std::filesystem::path temp =
        file.string() + "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp";
    {
        const std::string text = performanceText(record);
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), std::streamsize(text.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(temp, ec);
            error = file.string() + ": couldn't write it";
            return false;
        }
    }
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        error = file.string() + ": " + ec.message();
        std::filesystem::remove(temp, ec);
        return false;
    }
    error.clear();
    return true;
}

PerformanceRecords readPerformanceRecords(const Project& project) {
    PerformanceRecords out;
    const std::filesystem::path folder = performanceFolder(project);
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) return out;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".swperf") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (const std::filesystem::path& file : files) {
        const std::string shown = project.relative(file);
        PerformanceRecord record;
        Diagnostic error;
        if (readPerformance(file, shown, record, error)) {
            out.records.push_back(std::move(record));
            out.files.push_back(shown);
        } else {
            out.problems.push_back(error);
        }
    }
    return out;
}

// ---- The gate ---------------------------------------------------------------------------

uint64_t memoryGrowthAllowanceMib(const ReleaseSettings& contract) {
    return std::max<uint64_t>(32, contract.cpuBudgetMib / 20);
}

namespace {

int tierRank(std::string_view tier) {
    constexpr const char* names[] = {"low", "medium", "high", "ultra"};
    for (int i = 0; i < 4; ++i)
        if (tier == names[i]) return i;
    return -1;   // "custom" or unknown
}

bool optimised(std::string_view config) {
    return config == "Release" || config == "RelWithDebInfo" || config == "MinSizeRel";
}

// Problems that mean the record can't count at all, whatever its numbers.
bool unusable(const PerformanceRecord& r, std::string& why) {
    if (r.renderer != "vulkan" || r.gpuKind == "none")
        return why = "it ran on the " + (r.renderer.empty() ? std::string("unknown") : r.renderer) +
                     " renderer, which draws nothing",
               true;
    if (r.gpuKind == "software" || r.gpuKind == "virtual")
        return why = "it ran on " + r.gpu + ", a " + r.gpuKind + " GPU; only real hardware counts", true;
    return false;
}

std::string ms(double value) { return number(std::round(value * 100) / 100) + " ms"; }

}   // namespace

std::vector<std::string>
performanceProblems(const ReleaseSettings& c, const PerformanceRecord& r, std::string_view currentBuildId) {
    std::vector<std::string> out;
    std::string why;
    if (unusable(r, why)) out.push_back(why);
    if (!optimised(r.buildConfig))
        out.push_back(
            "it's from a " + (r.buildConfig.empty() ? std::string("build of unknown type") : r.buildConfig + " build") +
            "; releases are measured on an optimised one");
    if (!currentBuildId.empty() && r.buildId != currentBuildId)
        out.push_back(
            r.buildId.empty()
                ? std::string("it names no build, and the game is now build ") + std::string(currentBuildId)
                : "it's from build " + r.buildId + "; the game is now " + std::string(currentBuildId));
    const int promised = int(c.graphicsTier);
    const int measured = tierRank(r.tier);
    if (measured < promised)
        out.push_back(
            "it ran at " + (r.tier.empty() ? std::string("unknown") : r.tier) + " graphics; the contract promises " +
            releaseGraphicsTierName(c.graphicsTier));
    if (r.width * r.height < 1280u * 720u)
        out.push_back(
            "it drew at " + std::to_string(r.width) + "x" + std::to_string(r.height) +
            ", under 1280x720; measure at the resolution players get");
    if (r.frameLimit > 0 && r.frameLimit < c.targetFps)
        out.push_back(
            "its frame limit " + number(r.frameLimit) + " is under the target " + std::to_string(c.targetFps) + " fps");
    if (r.play.frames == 0) out.push_back("it has no frames of play");
    if (r.play.frames > 0) {
        if (r.play.p50Ms > c.frameBudgetMs())
            out.push_back(
                "half the frames took over " + ms(r.play.p50Ms) + ", more than the frame budget " +
                ms(c.frameBudgetMs()) + " (" + std::to_string(c.targetFps) + " fps)");
        if (c.frameP95Ms > 0 && r.play.p95Ms > c.frameP95Ms)
            out.push_back("frame p95 " + ms(r.play.p95Ms) + " is over the budget " + ms(c.frameP95Ms));
        if (c.frameP99Ms > 0 && r.play.p99Ms > c.frameP99Ms)
            out.push_back("frame p99 " + ms(r.play.p99Ms) + " is over the budget " + ms(c.frameP99Ms));
        if (c.maxHitchMs > 0 && r.play.worstMs > c.maxHitchMs)
            out.push_back("the worst frame " + ms(r.play.worstMs) + " is over max_hitch_ms " + ms(c.maxHitchMs));
    }
    for (const PerformanceLoad& load : r.loads)
        if (c.levelLoadSeconds > 0 && load.seconds > c.levelLoadSeconds) {
            out.push_back(
                "loading " + load.level + " took " + number(std::round(load.seconds * 100) / 100) +
                " s, over level_load_seconds " + number(c.levelLoadSeconds));
            break;
        }
    if (r.cpuPeakMib > c.cpuBudgetMib)
        out.push_back(
            "it held " + std::to_string(r.cpuPeakMib) + " MiB, over cpu_budget_mib " + std::to_string(c.cpuBudgetMib));
    if (r.gpuPeakMib > c.gpuBudgetMib)
        out.push_back(
            "the GPU held " + std::to_string(r.gpuPeakMib) + " MiB, over gpu_budget_mib " +
            std::to_string(c.gpuBudgetMib));
    if (c.sharedBudgetMib > 0 && r.cpuPeakMib + r.gpuPeakMib > c.sharedBudgetMib)
        out.push_back(
            "CPU and GPU together held " + std::to_string(r.cpuPeakMib + r.gpuPeakMib) +
            " MiB, over shared_budget_mib " + std::to_string(c.sharedBudgetMib));
    if (r.memoryGrowthMib > int64_t(memoryGrowthAllowanceMib(c)))
        out.push_back(
            "memory grew " + std::to_string(r.memoryGrowthMib) + " MiB over " + std::to_string(r.laps) +
            " laps (allowed " + std::to_string(memoryGrowthAllowanceMib(c)) + "): something isn't freed");
    if (r.pipelinesInPlay > 0)
        out.push_back(std::to_string(r.pipelinesInPlay) + " pipelines were built during play instead of while loading");
    return out;
}

PerformanceGate performanceGate(
    const ReleaseSettings& contract, std::span<const PerformanceRecord> records, std::span<const std::string> files,
    std::string_view currentBuildId) {
    PerformanceGate gate;
    auto name = [&](size_t i) {
        const std::string machine = records[i].machine.empty() ? std::string("a machine") : records[i].machine;
        return machine + (i < files.size() ? " (" + files[i] + ")" : std::string());
    };
    if (!contract.hasPerformanceBudgets()) {
        gate.verdict = PerformanceVerdict::Failed;
        gate.reason = "the contract states no performance budgets: add frame_p95_ms, frame_p99_ms, max_hitch_ms and "
                      "level_load_seconds (shockwave_release 2)";
        return gate;
    }
    if (records.empty()) {
        gate.reason = "no performance record yet: run player --perf-run <scenario> --perf-record \"<machine>\" on "
                      "the minimum hardware, in an optimised build";
        return gate;
    }
    // A record that passes wins. Otherwise the gate fails on a usable record (real hardware,
    // optimised, this build) that misses a budget, and stays unverified when every record
    // is unusable.
    int failing = -1, unusableRecord = -1;
    std::vector<std::string> failingProblems, unusableProblems;
    for (size_t i = 0; i < records.size(); ++i) {
        const std::vector<std::string> problems = performanceProblems(contract, records[i], currentBuildId);
        if (problems.empty()) {
            gate.verdict = PerformanceVerdict::Passed;
            gate.record = int(i);
            const PerformanceRecord& r = records[i];
            gate.reason = name(i) + ": " + r.gpu + ", " + std::to_string(r.width) + "x" + std::to_string(r.height) +
                          " at " + r.tier + ", p95 " + ms(r.play.p95Ms) + ", p99 " + ms(r.play.p99Ms) + ", worst " +
                          ms(r.play.worstMs) + ", every budget met";
            return gate;
        }
        std::string why;
        const bool stale = !currentBuildId.empty() && records[i].buildId != currentBuildId;
        if (unusable(records[i], why) || !optimised(records[i].buildConfig) || stale) {
            if (unusableRecord < 0) unusableRecord = int(i), unusableProblems = problems;
        } else if (failing < 0) {
            failing = int(i), failingProblems = problems;
        }
    }
    auto join = [](const std::vector<std::string>& problems) {
        std::string out;
        for (size_t i = 0; i < problems.size(); ++i) out += (i ? "; " : "") + problems[i];
        return out;
    };
    if (failing >= 0) {
        gate.verdict = PerformanceVerdict::Failed;
        gate.record = failing;
        gate.reason = name(size_t(failing)) + ": " + join(failingProblems);
    } else {
        gate.record = unusableRecord;
        gate.reason = "no record counts yet. " + name(size_t(unusableRecord)) + ": " + join(unusableProblems);
    }
    return gate;
}

}   // namespace shockwave
