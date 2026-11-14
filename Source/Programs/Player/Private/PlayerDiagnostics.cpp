// Diagnostics in the player: log files, crash capture, the session marker, --diagnostics,
// --symbolize and --safe-mode (docs/Diagnostics.md, S12 of the shipping plan).
#include "Player.h"

#include <Core/Diagnostics.h>
#include <Core/Log.h>
#include <Core/Process.h>
#include <Engine/ReleaseTier.h>
#include <Packaging/Package.h>
#include <Platform/UserPaths.h>
#include <Projects/Diagnostic.h>
#include <Projects/ReleaseSettings.h>

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace player {

namespace diag = shockwave::diagnostics;

namespace {

std::vector<std::string> g_commandLine;   // this run's arguments, for a restart

std::string str(const std::string& text) { return shockwave::diagnosticJsonString(text); }

std::string contextOf(const diag::PreviousSession& session, std::string_view key) {
    for (const auto& [k, value] : session.context)
        if (k == key) return value;
    return {};
}

// A dialog can be shown: not a headless run (tests, CI, a server) or a machine without a
// display.
bool displayAvailable() {
    const char* driver = std::getenv("SDL_VIDEO_DRIVER");
    if (driver && (std::string_view(driver) == "offscreen" || std::string_view(driver) == "dummy")) return false;
#if defined(__linux__)
    const char* x11 = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    if ((!x11 || !*x11) && (!wayland || !*wayland)) return false;
#endif
    return true;
}

// A system dialog with these buttons (the first is the default); the index pressed, or -1.
int ask(const std::string& title, const std::string& message, const std::vector<const char*>& choices) {
    if (!displayAvailable()) return -1;
    std::vector<SDL_MessageBoxButtonData> buttons;
    for (size_t i = 0; i < choices.size(); ++i) {
        SDL_MessageBoxButtonData b{};
        b.flags = i == 0 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT
                         : (i + 1 == choices.size() ? SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT : 0);
        b.buttonID = int(i);
        b.text = choices[i];
        buttons.push_back(b);
    }
    SDL_MessageBoxData data{};
    data.flags = SDL_MESSAGEBOX_WARNING | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
    data.title = title.c_str();
    data.message = message.c_str();
    data.numbuttons = int(buttons.size());
    data.buttons = buttons.data();
    int pressed = -1;
    if (!SDL_ShowMessageBox(&data, &pressed)) return -1;
    return pressed;
}

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
        const std::string level = contextOf(previous, "level");
        SW_LOG(
            shockwave::LogEngine, Warning,
            "The last run (started {}{}) didn't end cleanly{}{}. Start with --safe-mode for safe graphics",
            previous.startedAt, level.empty() ? std::string() : ", level " + level,
            contextOf(previous, "renderer_state") == "device lost" ? " (its GPU device was lost)" : "",
            previous.crashReport.empty() ? std::string() : "; its report is " + shown(previous.crashReport));
    }
}

void endRunDiagnostics() {
    diag::endSession();
    diag::closeLogFile();
}

void setCommandLine(int argc, char** argv) { g_commandLine.assign(argv + 1, argv + argc); }

bool needsStartupRecovery() {
    const diag::PreviousSession& previous = diag::previousSession();
    if (!previous.found || previous.stillRunning) return false;
    // It never showed a frame (it may crash in the same place again before any menu could
    // offer anything), or its GPU device was lost.
    return contextOf(previous, "first_frame") != "yes" || contextOf(previous, "renderer_state") == "device lost";
}

bool askSafeModeAtStart() {
    const diag::PreviousSession& previous = diag::previousSession();
    const bool lost = contextOf(previous, "renderer_state") == "device lost";
    const std::string game = contextOf(previous, "project").empty() ? "The game" : contextOf(previous, "project");
    const std::string message = (lost ? game + "'s graphics device stopped responding last time"
                                      : game + " stopped before it showed anything last time") +
                                " (started " + previous.startedAt +
                                ").\n\nSafe graphics start it with the lowest settings, in a window, "
                                "without the shader cache. Your own settings and saves stay as they are.";
    const int choice = ask(game, message, {"Start with safe graphics", "Start normally"});
    SW_LOG(
        shockwave::LogEngine, Info, "After the last run: {}",
        choice == 0   ? "starting with safe graphics"
        : choice == 1 ? "starting normally"
                      : "no dialog shown");
    return choice == 0;
}

bool restartPlayer(bool safe, std::string& error) {
    shockwave::ProcessOptions again;
    again.executable = shockwave::currentExecutable();
    std::error_code ec;
    again.workingDirectory = fs::current_path(ec);
    for (size_t i = 0; i < g_commandLine.size(); ++i) {
        const std::string& a = g_commandLine[i];
        if (a == "--safe-mode") continue;
        if (a == "--inject-device-lost") {   // a test's fault happens once
            ++i;
            continue;
        }
        again.arguments.push_back(a);
    }
    if (safe) again.arguments.push_back("--safe-mode");
    diag::endSession();   // so the new run doesn't take this one for a crash
    if (!shockwave::startDetachedProcess(again, error)) return false;
    SW_LOG(shockwave::LogEngine, Info, "Restarting: {}", shockwave::describeProcess(again));
    return true;
}

std::optional<shockwave::AppShell::Setup::Recovery> Player::recovery() {
    if (!m_tool.recoveryPrompt || !m_tool.userPaths) return std::nullopt;
    // A game run read the marker as it started; a flow test (which keeps none of its own)
    // only looks, so scenarios can test this page.
    diag::PreviousSession previous = diag::previousSession();
    if (!previous.found && !m_tool.flowTest.empty())
        previous = diag::inspectSession(m_tool.userPaths->logs(), m_tool.userPaths->crashes(), "player");
    if (!previous.found || previous.stillRunning) return std::nullopt;
    shockwave::AppShell::Setup::Recovery r;
    r.started = previous.startedAt;
    r.level = contextOf(previous, "level");
    r.report = previous.crashReport.empty() ? std::string() : previous.crashReport.filename().string();
    r.deviceLost = contextOf(previous, "renderer_state") == "device lost";
    if (!m_tool.safeMode)
        r.safeGraphics = [this](std::string& why) {
            // For this session: applied, not saved. The player's file changes only if they
            // apply settings themselves.
            const shockwave::GraphicsSettings safe = safeGraphics(m_project, engine().graphics().current());
            if (!engine().graphics().apply(safe, why)) return false;
            diag::setContext("safe_mode", "yes (from the menu)");
            SW_LOG(shockwave::LogEngine, Warning, "Safe graphics for this session (chosen after the last run)");
            return true;
        };
    const fs::path logs = m_tool.userPaths->logs();
    r.openLogs = [logs](std::string& why) {
        std::error_code ec;
        if (!fs::is_directory(logs, ec)) {
            why = "There are no log files yet";
            return false;
        }
        if (!SDL_OpenURL(("file://" + fs::absolute(logs, ec).generic_string()).c_str())) {
            why = std::string("Couldn't open the folder: ") + SDL_GetError();
            return false;
        }
        return true;
    };
    SW_LOG(shockwave::LogEngine, Info, "The menu offers recovery after the last run (started {})", r.started);
    return r;
}

void Player::onRendererLost(const std::string& reason) {
    // Nothing more is simulated or saved: the last save stays the last good one.
    SW_LOG(
        shockwave::LogEngine, Error,
        "{}. The game has stopped; your saves are as they were. Start it again (--safe-mode for safe graphics)",
        reason);
    m_exitCode = 1;
    const std::string game = m_project.name.empty() ? "The game" : m_project.name;
    const int choice =
        ask(game,
            game + "'s graphics device stopped responding (" + reason +
                "), so it can't go on drawing.\n\nYour saved games are as they were at the last save.",
            {"Restart with safe graphics", "Restart", "Quit"});
    if (choice == 0 || choice == 1) {
        std::string error;
        if (!restartPlayer(choice == 0, error)) SW_LOG(shockwave::LogEngine, Error, "Couldn't restart: {}", error);
    } else {
        // The next start sees this run didn't end cleanly and offers safe graphics.
        diag::setContext("renderer_state", "device lost");
        diag::leaveSession();
    }
    engine().quit();
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
