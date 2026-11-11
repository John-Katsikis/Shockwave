// Diagnostics (Core/Diagnostics.h, docs/Diagnostics.md): the recent-log ring, the session
// context, redaction, rotating log files (and a full disk), the session marker that tells
// an unclean exit, finding crash reports, and naming frames from ELF symbols. The crashes
// themselves are in Tests/DiagnosticsCheck.cmake, run in a child process.
#include <Core/Diagnostics.h>
#include <Core/Log.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#if defined(__linux__)
#include <cstdint>
#endif

namespace fs = std::filesystem;
namespace diag = shockwave::diagnostics;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace {

SW_LOG_CATEGORY(LogTest);

std::string readFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool ringHas(std::string_view text) {
    for (const std::string& line : diag::recentLog())
        if (line.find(text) != std::string::npos) return true;
    return false;
}

void ring() {
    SW_LOG(LogTest, Info, "first line {}", 1);
    SW_LOG(LogTest, Warning, "a warning");
    CHECK(ringHas("LogTest: first line 1"));
    CHECK(ringHas("LogTest: warning: a warning"));
    // Past 64 KiB the oldest lines go; the ring never grows.
    for (int i = 0; i < 2000; ++i) SW_LOG(LogTest, Info, "filler {:04} {}", i, std::string(60, 'x'));
    CHECK(!ringHas("first line 1"));
    CHECK(ringHas("filler 1999"));
    size_t bytes = 0;
    for (const std::string& line : diag::recentLog()) bytes += line.size() + 1;
    CHECK(bytes <= diag::kRingBytes);
    CHECK(bytes > diag::kRingBytes - 200);
}

void context() {
    diag::setContext("level", "factory");
    CHECK(diag::contextValue("level") == "factory");
    diag::setContext("level", "ending");
    CHECK(diag::contextValue("level") == "ending");
    diag::setContext("level", "");
    CHECK(diag::contextValue("level").empty());
    diag::setContext("multi", "one\ntwo\rthree");
    CHECK(diag::contextValue("multi") == "one two three");
    diag::setContext("long", std::string(1000, 'v'));
    CHECK(diag::contextValue("long").size() == diag::kMaxContextValueBytes);
    // At most kMaxContextKeys; extra keys are dropped, existing ones still change.
    for (size_t i = 0; i < diag::kMaxContextKeys + 10; ++i) diag::setContext("k" + std::to_string(i), "v");
    CHECK(diag::context().size() == diag::kMaxContextKeys);
    diag::setContext("multi", "changed");
    CHECK(diag::contextValue("multi") == "changed");
    for (size_t i = 0; i < diag::kMaxContextKeys + 10; ++i) diag::setContext("k" + std::to_string(i), "");
    diag::setContext("long", "");
    diag::setContext("multi", "");
    // An error leaves its message as last_error.
    SW_LOG(LogTest, Error, "the factory didn't load");
    CHECK(diag::contextValue("last_error") == "LogTest: the factory didn't load");
}

void redaction() {
    diag::addRedaction("/home/alice", "~");
    CHECK(diag::redact("saved /home/alice/.local/share/x") == "saved ~/.local/share/x");
    const std::string ticket(120, 'b');
    CHECK(diag::redact("ticket " + ticket + " end") == "ticket [redacted] end");
    const std::string digest(64, 'c');   // a SHA-256 is fine to show
    CHECK(diag::redact(digest) == digest);
    SW_LOG(LogTest, Info, "auth {} for /home/alice/game", ticket);
    CHECK(ringHas("auth [redacted] for ~/game"));
    CHECK(!ringHas(ticket));
    diag::setContext("path", "/home/alice/game");
    CHECK(diag::contextValue("path") == "~/game");
    diag::setContext("path", "");
}

void logFiles(const fs::path& temp) {
    const fs::path folder = temp / "Logs";
    diag::LogFileOptions options;
    options.folder = folder;
    options.name = "game";
    options.keep = 2;
    for (int run = 0; run < 4; ++run) {
        std::string error;
        CHECK(diag::openLogFile(options, error));
        CHECK(diag::logFilePath() == folder / "game.log");
        SW_LOG(LogTest, Info, "run {}", run);
        diag::closeLogFile();
    }
    CHECK(readFile(folder / "game.log").find("run 3") != std::string::npos);
    CHECK(readFile(folder / "game.1.log").find("run 2") != std::string::npos);
    CHECK(readFile(folder / "game.2.log").find("run 1") != std::string::npos);
    CHECK(!fs::exists(folder / "game.3.log"));
    CHECK(readFile(folder / "game.log").find("run 2") == std::string::npos);

    // Past maxBytes the file stops with a note; the ring goes on.
    options.maxBytes = 200;
    std::string error;
    CHECK(diag::openLogFile(options, error));
    for (int i = 0; i < 20; ++i) SW_LOG(LogTest, Info, "line {}", i);
    CHECK(diag::logFileStatus().stopped);
    CHECK(diag::logFileStatus().stopReason == "size limit");
    CHECK(readFile(folder / "game.log").find("size limit") != std::string::npos);
    CHECK(ringHas("line 19"));
    diag::closeLogFile();

#if defined(__linux__)
    // A full disk: the file stops with the system's reason, nothing else breaks.
    if (fs::exists("/dev/full")) {
        diag::LogFileOptions full;
        full.exactFile = "/dev/full";
        CHECK(diag::openLogFile(full, error));
        for (int i = 0; i < 200; ++i) SW_LOG(LogTest, Info, "to a full disk {} {}", i, std::string(100, 'y'));
        const diag::LogFileStatus status = diag::logFileStatus();
        CHECK(status.stopped);
        CHECK(status.stopReason.find("space") != std::string::npos);
        CHECK(!status.open);
        CHECK(ringHas("to a full disk 199"));
        diag::closeLogFile();
    }
#endif
    // A folder that can't be made: a reason, and the ring still records.
    diag::LogFileOptions bad;
    bad.folder = folder / "game.log" / "inside a file";
    CHECK(!diag::openLogFile(bad, error));
    CHECK(!error.empty());
}

void sessions(const fs::path& temp) {
    const fs::path logs = temp / "SessionLogs", crashes = temp / "SessionCrashes";
    std::string error;
    diag::PreviousSession previous;
    CHECK(diag::beginSession(logs, crashes, "game", previous, error));
    CHECK(!previous.found);
    CHECK(fs::exists(logs / "game.session"));
    diag::endSession();   // a clean exit
    CHECK(!fs::exists(logs / "game.session"));
    CHECK(!diag::inspectSession(logs, crashes, "game").found);

    // A run that didn't end: its marker stays, with its context.
    diag::setContext("level", "factory");
    CHECK(diag::beginSession(logs, crashes, "game", previous, error));
    diag::updateSession();
    fs::create_directories(crashes);
    std::ofstream(crashes / "game-20261111-120000-1.crash.txt")
        << "shockwave_crash 1\nprogram game\nsignal SIGSEGV (11)\n";
    std::ofstream(crashes / "other-20261111-120000-1.crash.txt") << "shockwave_crash 1\n";
    diag::PreviousSession found = diag::inspectSession(logs, crashes, "game");
    CHECK(found.found);
    CHECK(!found.stillRunning);   // this very process wrote it
    CHECK(found.program == "game");
    CHECK(found.startedAt.size() == 20);
    bool hasLevel = false;
    for (const auto& [key, value] : found.context) hasLevel |= key == "level" && value == "factory";
    CHECK(hasLevel);
    CHECK(found.crashReport.filename() == "game-20261111-120000-1.crash.txt");
    // The next start sees it, and writes its own.
    CHECK(diag::beginSession(logs, crashes, "game", previous, error));
    CHECK(previous.found);
    CHECK(diag::previousSession().found);
    diag::endSession();
    diag::setContext("level", "");

#if !defined(_WIN32)
    // A marker whose process is alive (process 1): another copy of the game. Its marker
    // stays and this run writes game.<pid>.session.
    std::ofstream(logs / "game.session") << "shockwave_session 1\nstarted 2026-11-11T00:00:00Z\nprogram game\npid 1\n";
    CHECK(diag::beginSession(logs, crashes, "game", previous, error));
    CHECK(previous.found && previous.stillRunning);
    CHECK(fs::exists(logs / "game.session"));
    size_t markers = 0;
    for (const auto& entry : fs::directory_iterator(logs)) markers += entry.path().extension() == ".session";
    CHECK(markers == 2);
    diag::endSession();
    CHECK(fs::exists(logs / "game.session"));
    fs::remove(logs / "game.session");
#endif

    // Reports, newest first.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::ofstream(crashes / "game-20261111-130000-2.crash.txt") << "shockwave_crash 1\n";
    const auto reports = diag::crashReports(crashes);
    CHECK(reports.size() == 3);
    CHECK(!reports.empty() && reports.front().filename() == "game-20261111-130000-2.crash.txt");
    CHECK(diag::crashReports(temp / "nowhere").empty());
}

void crashHandler(const fs::path& temp) {
    std::string error;
    CHECK(diag::crashCaptureSupported());
    CHECK(!diag::installCrashHandler({{}, "x"}, error));   // no folder
    CHECK(diag::installCrashHandler({temp / "Crashes", "unit"}, error));
    CHECK(diag::crashHandlerInstalled());
    CHECK(diag::pendingCrashReport().filename().string().rfind("unit-", 0) == 0);
    CHECK(diag::pendingCrashReport().extension() == ".txt");
    CHECK(!fs::exists(temp / "Crashes"));   // made only when a crash happens
    diag::uninstallCrashHandler();
    CHECK(!diag::crashHandlerInstalled());
}

#if defined(__linux__)
// A function to find by address.
__attribute__((noinline)) int diagnosticsTestMarker(int x) { return x * 3 + 1; }

void symbolization(const fs::path& temp) {
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    CHECK(diag::elfBuildId(self).size() >= 16);
    CHECK(diag::elfBuildId(temp / "missing").empty());
    // A report shaped like the crash handler's, from this process's own maps.
    const auto address = reinterpret_cast<uintptr_t>(&diagnosticsTestMarker);
    std::ostringstream report;
    report << "shockwave_crash 1\nprogram unit\nsignal SIGSEGV (11)\n[context]\nlevel: factory\n[frames]\n";
    report << "0x" << std::hex << address + 1 << "\n0x10\n";
    report << "[modules]\n" << readFile("/proc/self/maps") << "[log]\nLogTest: hello\n[end]\n";
    const fs::path file = temp / "unit.crash.txt";
    std::ofstream(file) << report.str();
    diag::CrashReport out;
    std::string error;
    CHECK(diag::symbolizeCrashReport(file, {}, out, error));
    CHECK(out.program == "unit");
    CHECK(out.signal == "SIGSEGV (11)");
    CHECK(out.frames.size() == 2);
    CHECK(!out.frames.empty() && out.frames[0].function.find("diagnosticsTestMarker") != std::string::npos);
    CHECK(out.frames.size() == 2 && out.frames[1].function.empty() && out.frames[1].module.empty());
    CHECK(out.context.size() == 1 && out.recentLog.size() == 1);
    std::ofstream(temp / "not-a-report.txt") << "hello\n";
    CHECK(!diag::symbolizeCrashReport(temp / "not-a-report.txt", {}, out, error));
    CHECK(error.find("isn't a Shockwave crash report") != std::string::npos);
    CHECK(diagnosticsTestMarker(1) == 4);
}
#endif

}   // namespace

int main() {
    const fs::path temp = fs::temp_directory_path() / "shockwave_diagnostics_test";
    fs::remove_all(temp);
    fs::create_directories(temp);
    shockwave::setLogToStderr(false);
    diag::startRecording();
    diag::startRecording();   // idempotent
    CHECK(diag::recording());
    ring();
    context();
    redaction();
    logFiles(temp);
    sessions(temp);
    crashHandler(temp);
#if defined(__linux__)
    symbolization(temp);
#endif
    diag::stopRecording();
    CHECK(!diag::recording());
    fs::remove_all(temp);
    if (failures) {
        std::fprintf(stderr, "diagnostics: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("diagnostics: all checks passed\n");
    return 0;
}
