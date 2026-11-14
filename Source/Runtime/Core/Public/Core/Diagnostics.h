#pragma once

// Diagnostics: what a player's machine keeps so a failure can be understood later
// (docs/Diagnostics.md, S12 of the shipping plan).
//
//   - A recent-log ring: the last 64 KiB of log lines, always in memory, so a crash report
//     carries what happened just before it.
//   - A session context: key/value pairs the engine keeps current (build, level, epoch,
//     travel phase, renderer, GPU, driver, graphics tier, last save, last error).
//   - Log files that rotate in the per-user Logs folder: <name>.log for this run, the
//     previous runs as <name>.1.log ... <name>.N.log, never beside the game.
//   - Redaction: the user's home folder is written as "~", and long hex runs (tickets,
//     tokens) as "[redacted]", in the files, the ring and reports. Stderr is unchanged.
//   - Crash capture: a fatal signal (POSIX) or unhandled exception (Windows) writes a report
//     to the Crashes folder, from a handler that only uses async-signal-safe calls and
//     memory set up beforehand (Windows adds a minidump). Nothing is ever uploaded.
//   - A session marker: written at start, removed at a clean exit; still there at the next
//     start means the previous run didn't end cleanly.
//   - Symbolization: turns a report's raw addresses back into function names, from the
//     binaries or from the symbols kept beside a package (<package>-symbols/<build-id>/).
//
// Everything here is plain files and memory: no network, no UI. The player and the editor
// start it; tests use their own folders. Faults for tests are injected by the test program
// (Tests/CrashProgram.cpp), never by anything a shipped game contains.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shockwave::diagnostics {

// ---- Recording: the ring, the context and redaction ---------------------------------------

// Starts recording log lines into the ring (idempotent). Also learns the home folder to
// redact. Costs a copy of each line.
void startRecording();
void stopRecording();
bool recording();

inline constexpr size_t kRingBytes = 64 * 1024;
// The ring's lines, oldest first ("LogGame: warning: ..."). The oldest may be cut short.
std::vector<std::string> recentLog();

// Sets (or, with an empty value, removes) a context value. Keys are short snake_case.
// Thread-safe; values are redacted as they're stored.
void setContext(std::string_view key, std::string_view value);
std::string contextValue(std::string_view key);
std::vector<std::pair<std::string, std::string>> context();   // sorted by key
inline constexpr size_t kMaxContextKeys = 48;
inline constexpr size_t kMaxContextValueBytes = 256;

// Adds text to replace wherever it appears (the home folder is added by startRecording).
void addRedaction(std::string_view text, std::string_view replacement);
std::string redact(std::string_view text);

// ---- Log files ------------------------------------------------------------------------------

struct LogFileOptions {
    std::filesystem::path folder;   // the per-user Logs folder; created if missing
    std::string name = "player";   // <name>.log
    int keep = 5;   // previous runs kept, <name>.1.log (newest) .. <name>.<keep>.log
    uint64_t maxBytes = 16ull << 20;   // past this, the file stops with a note (the ring goes on)
    // Write here instead of <folder>/<name>.log, without rotating (tests: /dev/full).
    std::filesystem::path exactFile;
};

// Rotates the old files and opens this run's log. False with a reason when the folder or
// file can't be made; the ring keeps recording either way.
bool openLogFile(const LogFileOptions& options, std::string& error);
void closeLogFile();
std::filesystem::path logFilePath();   // empty when no file is open

struct LogFileStatus {
    bool open = false;
    bool stopped = false;   // full disk, a write error or maxBytes: no more lines go in
    std::string stopReason;
    uint64_t bytes = 0;
};
LogFileStatus logFileStatus();

// ---- Session marker -------------------------------------------------------------------------

struct PreviousSession {
    bool found = false;   // a marker was there: the previous run didn't end cleanly
    bool stillRunning = false;   // its process is alive (another copy of the game is running)
    std::string startedAt;   // UTC, ISO 8601
    std::string program;
    int64_t pid = 0;
    std::vector<std::pair<std::string, std::string>> context;   // the context it last wrote
    std::filesystem::path crashReport;   // the newest crash report written after it started
};

// Reads the marker the previous run left, without writing anything.
PreviousSession
inspectSession(const std::filesystem::path& folder, const std::filesystem::path& crashes, std::string_view program);
// Reads the marker left by the previous run, then writes this run's. `folder` is the
// per-user Logs folder, `crashes` the Crashes folder (to find that run's report).
bool beginSession(
    const std::filesystem::path& folder, const std::filesystem::path& crashes, std::string_view program,
    PreviousSession& previous, std::string& error);
// Writes the context into the marker again (call after big changes; cheap).
void updateSession();
// A clean exit: removes the marker.
void endSession();
// Writes the marker one last time and leaves it in place, so the next run sees this one as
// not having ended cleanly (it quit after the GPU device was lost, say) and offers safe
// graphics. endSession does nothing after it.
void leaveSession();
// The previous run, as beginSession found it (empty before it ran).
const PreviousSession& previousSession();

// ---- Crash capture --------------------------------------------------------------------------

struct CrashOptions {
    std::filesystem::path folder;   // the per-user Crashes folder; made only when a crash happens
    std::string program;   // "player", "editor"
    bool minidump = true;   // Windows: also write a .dmp beside the report
};

// Installs the handlers. False (with a reason) without a folder or on a platform with no
// support. Re-installing replaces the options.
bool installCrashHandler(const CrashOptions& options, std::string& error);
void uninstallCrashHandler();
bool crashHandlerInstalled();
// The file the next crash would write.
std::filesystem::path pendingCrashReport();
// Whether this platform can capture crashes at all (Linux, macOS, Windows).
bool crashCaptureSupported();

// The crash reports in a folder, newest first.
std::vector<std::filesystem::path> crashReports(const std::filesystem::path& folder);

// ---- Symbolization --------------------------------------------------------------------------

struct Frame {
    int index = 0;
    uint64_t address = 0;   // as captured
    std::string module;   // the binary's path as it was when it crashed
    uint64_t offset = 0;   // the address in that binary's own numbering
    std::string function;   // demangled; empty when unknown
    std::string symbolFile;   // where the name came from
};

struct CrashReport {
    std::string program, signal, startedAt;
    std::vector<std::pair<std::string, std::string>> context;
    std::vector<std::string> recentLog;
    std::vector<Frame> frames;
};

// Reads a report and names each frame from the binaries' symbol tables: first in each
// symbol folder (<dir>/<build-id>/<file name>), then the module where it was. False with
// a reason for a file that isn't a report.
bool symbolizeCrashReport(
    const std::filesystem::path& report, const std::vector<std::filesystem::path>& symbolFolders, CrashReport& out,
    std::string& error);

// The GNU build ID of an ELF binary as hex; empty when it has none or isn't ELF.
std::string elfBuildId(const std::filesystem::path& binary);
// The function containing `offset` (a virtual address in that binary), from its .symtab or
// .dynsym; empty when none does.
std::string elfFunctionAt(const std::filesystem::path& binary, uint64_t offset);

}   // namespace shockwave::diagnostics
