// Diagnostics in the player: log files, crash capture, the session marker, --diagnostics,
// --symbolize and --safe-mode (docs/Diagnostics.md, S12 of the shipping plan).
#include "Player.h"

#include <Core/Diagnostics.h>
#include <Core/Log.h>
#include <Engine/ReleaseTier.h>
#include <Packaging/Package.h>
#include <Platform/UserPaths.h>
#include <Projects/Diagnostic.h>
#include <Projects/ReleaseSettings.h>

#include <cstdio>
#include <fstream>

namespace player {

namespace diag = shockwave::diagnostics;

namespace {

std::string str(const std::string& text) { return shockwave::diagnosticJsonString(text); }

std::string shown(const fs::path& path) { return diag::redact(path.generic_string()); }

// The "signal" line of a report, without reading the rest.
std::string reportSignal(const fs::path& report) {
    std::ifstream in(report, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("signal ", 0) == 0) return line.substr(7);
        if (!line.empty() && line.front() == '[') break;
    }
    return "?";
}

int writeJson(const std::string& out, const fs::path& json) {
    if (json == "-") {
        std::fputs(out.c_str(), stdout);
        return 0;
    }
    std::ofstream file(json, std::ios::binary);
    file << out;
    file.close();
    if (!file) {
        std::fprintf(stderr, "Player: can't write %s\n", json.string().c_str());
        return 1;
    }
    std::printf("Wrote %s\n", json.string().c_str());
    return 0;
}

}   // namespace

void startDiagnostics() {
    diag::startRecording();
    diag::setContext("program", "player");
    diag::setContext(
        "engine", std::string(shockwave::engineVersion()) + " " +
                      (*shockwave::engineBuildConfig() ? shockwave::engineBuildConfig() : "no build type"));
    diag::setContext("platform", shockwave::packagePlatform());
    diag::setContext("installed", packagedProjectFile().empty() ? "no" : "yes");
}

void beginRunDiagnostics(
    const shockwave::Project& project, const shockwave::UserPaths* paths, const DiagnosticsCommand& command,
    bool gameRun) {
    diag::setContext("project", project.name);
    diag::setContext("application_id", shockwave::projectApplicationId(project));
    if (const std::string build = currentBuildId(project); !build.empty()) diag::setContext("build_id", build);
    if (command.safeMode) diag::setContext("safe_mode", "yes");
    std::string error;
    if (paths && !command.noCrashHandler) {
        if (!diag::installCrashHandler({paths->crashes(), "player"}, error))
            SW_LOG(shockwave::LogEngine, Warning, "No crash reports this run: {}", error);
    }
    if (!command.noLogFile) {
        diag::LogFileOptions options;
        if (!command.logFile.empty()) {
            options.exactFile = command.logFile;
        } else if (gameRun && paths) {
            options.folder = paths->logs();
        }
        if ((!options.exactFile.empty() || !options.folder.empty()) && !diag::openLogFile(options, error))
            SW_LOG(shockwave::LogEngine, Warning, "No log file this run: {}", error);
    }
    if (!gameRun || !paths) return;
    diag::PreviousSession previous;
    if (!diag::beginSession(paths->logs(), paths->crashes(), "player", previous, error)) {
        SW_LOG(shockwave::LogEngine, Warning, "Can't tell whether the next run follows a crash: {}", error);
        return;
    }
    if (previous.found && !previous.stillRunning) {
        std::string level;
        for (const auto& [key, value] : previous.context)
            if (key == "level") level = value;
        SW_LOG(
            shockwave::LogEngine, Warning,
            "The last run (started {}{}) didn't end cleanly{}. Start with --safe-mode for safe graphics",
            previous.startedAt, level.empty() ? std::string() : ", level " + level,
            previous.crashReport.empty() ? std::string() : "; its report is " + shown(previous.crashReport));
    }
}

void endRunDiagnostics() {
    diag::endSession();
    diag::closeLogFile();
}

shockwave::GraphicsSettings safeGraphics(const shockwave::Project& project, const shockwave::GraphicsSettings& base) {
    // The lowest tier the game says it supports, else the Low preset.
    const shockwave::ReleaseTierReport tier = shockwave::releaseTierReport(project);
    shockwave::GraphicsSettings safe = tier.readable ? shockwave::fallbackTierSettings(tier.tier, base)
                                                     : shockwave::graphicsPreset(shockwave::GraphicsPreset::Low, base);
    // Nothing a suspect driver might trip over that the game can do without.
    safe.renderScale = 1;
    safe.memoryBudgetMb = 0;
    return safe;
}

int diagnosticsInfo(
    const shockwave::Project& project, const shockwave::UserPaths* paths, const DiagnosticsCommand& command,
    const fs::path& json) {
    const std::string build = currentBuildId(project);
    const fs::path logs = paths ? paths->logs() : fs::path(), crashes = paths ? paths->crashes() : fs::path();
    const diag::PreviousSession previous =
        paths ? diag::inspectSession(logs, crashes, "player") : diag::PreviousSession{};
    const std::vector<fs::path> reports = paths ? diag::crashReports(crashes) : std::vector<fs::path>{};
    std::vector<fs::path> logFiles;
    std::error_code ec;
    if (paths)
        for (int n = 0; n <= 5; ++n) {
            const fs::path file = logs / ("player" + (n ? "." + std::to_string(n) : std::string()) + ".log");
            if (fs::is_regular_file(file, ec)) logFiles.push_back(file);
        }
    const char* previousState = !paths                  ? "unknown"
                                : !previous.found       ? "clean"
                                : previous.stillRunning ? "running"
                                                        : "unclean";
    (void)command;
    if (json.empty()) {
        std::printf(
            "program player\nengine %s %s (%s)\nproject %s\napplication_id %s\nbuild_id %s\n",
            shockwave::engineVersion(), shockwave::engineBuildConfig(), shockwave::packagePlatform(),
            project.name.c_str(), shockwave::projectApplicationId(project).c_str(),
            build.empty() ? "none (not cooked)" : build.c_str());
        if (!paths) {
            std::printf("user folders: none on this machine\n");
            return 1;
        }
        std::printf("logs %s\ncrashes %s\n", shown(logs).c_str(), shown(crashes).c_str());
        std::printf("crash capture %s\n", diag::crashCaptureSupported() ? "on" : "not on this platform");
        std::printf("log files %zu\n", logFiles.size());
        for (const fs::path& file : logFiles)
            std::printf(
                "  %s (%llu bytes)\n", file.filename().string().c_str(),
                static_cast<unsigned long long>(fs::file_size(file, ec)));
        std::printf("previous run %s", previousState);
        if (previous.found) {
            std::printf(" (started %s", previous.startedAt.c_str());
            for (const auto& [key, value] : previous.context)
                if (key == "level") std::printf(", level %s", value.c_str());
            std::printf(")");
        }
        std::printf("\n");
        if (!previous.crashReport.empty())
            std::printf("previous run's report %s\n", shown(previous.crashReport).c_str());
        std::printf("crash reports %zu\n", reports.size());
        for (size_t i = 0; i < reports.size() && i < 10; ++i)
            std::printf("  %s %s\n", reports[i].filename().string().c_str(), reportSignal(reports[i]).c_str());
        if (std::string(previousState) == "unclean")
            std::printf("advice: start with --safe-mode; player --symbolize <report> names the frames\n");
        return 0;
    }
    std::string out = "{\n  \"format\": 1,\n  \"mode\": \"diagnostics\",\n";
    out += "  \"engine\": " + str(shockwave::engineVersion()) + ",\n";
    out += "  \"build_config\": " + str(shockwave::engineBuildConfig()) + ",\n";
    out += "  \"platform\": " + str(shockwave::packagePlatform()) + ",\n";
    out += "  \"project\": " + str(project.name) + ",\n";
    out += "  \"application_id\": " + str(shockwave::projectApplicationId(project)) + ",\n";
    out += "  \"build_id\": " + str(build) + ",\n";
    out += "  \"user_folders\": " + std::string(paths ? "true" : "false") + ",\n";
    out += "  \"logs\": " + str(shown(logs)) + ",\n";
    out += "  \"crashes\": " + str(shown(crashes)) + ",\n";
    out += "  \"crash_capture\": " + std::string(diag::crashCaptureSupported() ? "true" : "false") + ",\n";
    out += "  \"log_files\": [";
    for (size_t i = 0; i < logFiles.size(); ++i)
        out += std::string(i ? ", " : "") + "{\"name\": " + str(logFiles[i].filename().string()) +
               ", \"bytes\": " + std::to_string(fs::file_size(logFiles[i], ec)) + "}";
    out += "],\n";
    out += "  \"previous_run\": {\"state\": " + str(previousState) + ", \"started\": " + str(previous.startedAt) +
           ", \"report\": " + str(previous.crashReport.empty() ? std::string() : shown(previous.crashReport)) +
           ", \"context\": {";
    for (size_t i = 0; i < previous.context.size(); ++i)
        out += std::string(i ? ", " : "") + str(previous.context[i].first) + ": " + str(previous.context[i].second);
    out += "}},\n  \"crash_reports\": [";
    for (size_t i = 0; i < reports.size() && i < 50; ++i)
        out += std::string(i ? ", " : "") + "{\"name\": " + str(reports[i].filename().string()) +
               ", \"signal\": " + str(reportSignal(reports[i])) + "}";
    out += "]\n}\n";
    return writeJson(out, json);
}

int symbolizeReport(const DiagnosticsCommand& command, const fs::path& json) {
    diag::CrashReport report;
    std::string error;
    if (!diag::symbolizeCrashReport(command.symbolize, command.symbols, report, error)) {
        std::fprintf(stderr, "Player: %s\n", error.c_str());
        return 1;
    }
    size_t named = 0;
    for (const diag::Frame& frame : report.frames) named += !frame.function.empty();
    if (json.empty()) {
        std::printf("%s crashed: %s\n", report.program.c_str(), report.signal.c_str());
        for (const diag::Frame& frame : report.frames) {
            const std::string module =
                frame.module.empty() ? std::string("?") : fs::path(frame.module).filename().string();
            std::printf(
                "  #%-2d %s  (%s +0x%llx)\n", frame.index, frame.function.empty() ? "??" : frame.function.c_str(),
                module.c_str(), static_cast<unsigned long long>(frame.offset));
        }
        std::printf("%zu of %zu frames named\n", named, report.frames.size());
        if (!report.context.empty()) {
            std::printf("context:\n");
            for (const auto& [key, value] : report.context) std::printf("  %s: %s\n", key.c_str(), value.c_str());
        }
        if (!report.recentLog.empty()) {
            std::printf("last log lines:\n");
            const size_t from = report.recentLog.size() > 20 ? report.recentLog.size() - 20 : 0;
            for (size_t i = from; i < report.recentLog.size(); ++i) std::printf("  %s\n", report.recentLog[i].c_str());
        }
        return 0;
    }
    std::string out = "{\n  \"format\": 1,\n  \"mode\": \"symbolize\",\n";
    out += "  \"program\": " + str(report.program) + ",\n  \"signal\": " + str(report.signal) + ",\n";
    out += "  \"named\": " + std::to_string(named) + ",\n  \"frames\": [";
    for (size_t i = 0; i < report.frames.size(); ++i) {
        const diag::Frame& f = report.frames[i];
        out += std::string(i ? ",\n    " : "\n    ") + "{\"index\": " + std::to_string(f.index) +
               ", \"function\": " + str(f.function) + ", \"module\": " + str(f.module) +
               ", \"offset\": " + std::to_string(f.offset) + ", \"symbols\": " + str(f.symbolFile) + "}";
    }
    out += "\n  ],\n  \"context\": {";
    for (size_t i = 0; i < report.context.size(); ++i)
        out += std::string(i ? ", " : "") + str(report.context[i].first) + ": " + str(report.context[i].second);
    out += "},\n  \"log_lines\": " + std::to_string(report.recentLog.size()) + "\n}\n";
    return writeJson(out, json);
}

}   // namespace player
