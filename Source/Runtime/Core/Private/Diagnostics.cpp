// The recent-log ring, session context, redaction, log files and the session marker. See
// Core/Diagnostics.h; crash capture is in CrashHandler.cpp, symbolization in Symbolize.cpp.
#include "DiagnosticsShared.h"

#include <Core/Log.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace shockwave::diagnostics {

namespace fs = std::filesystem;

namespace detail {
char g_ring[kRingBytes];
std::atomic<uint64_t> g_ringWritten{0};
char g_contextText[2][kContextTextBytes];
std::atomic<size_t> g_contextLength[2]{};
std::atomic<int> g_contextActive{0};
}   // namespace detail

namespace {

using namespace detail;

struct State {
    std::mutex ringMutex;   // the ring and the log file, taken inside the log's own lock
    int sink = 0;
    std::mutex contextMutex;   // context and redactions
    std::map<std::string, std::string, std::less<>> context;
    std::vector<std::pair<std::string, std::string>> redactions;
    // Log file.
    std::FILE* file = nullptr;
    fs::path filePath;
    LogFileOptions fileOptions;
    LogFileStatus fileStatus;
    // Session marker.
    fs::path markerPath;
    std::string program, startedAt;
    PreviousSession previous;
};

State& state() {
    static State instance;
    return instance;
}

int64_t processId() {
#if defined(_WIN32)
    return int64_t(GetCurrentProcessId());
#else
    return int64_t(getpid());
#endif
}

bool processAlive(int64_t pid) {
    if (pid <= 0 || pid == processId()) return false;
#if defined(_WIN32)
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!process) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
#else
    return kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

std::optional<std::string> environment(const char* name) {
    if (const char* value = std::getenv(name); value && *value) return std::string(value);
    return std::nullopt;
}

bool hexDigit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// Long runs of hex digits are tickets, tokens or keys, never something a person needs.
constexpr size_t kSecretHexRun = 96;

std::string redactLocked(const State& s, std::string_view text) {
    std::string out(text);
    for (const auto& [from, to] : s.redactions) {
        if (from.empty()) continue;
        for (size_t at = out.find(from); at != std::string::npos; at = out.find(from, at + to.size()))
            out.replace(at, from.size(), to);
    }
    for (size_t i = 0; i < out.size();) {
        if (!hexDigit(out[i])) {
            ++i;
            continue;
        }
        size_t end = i;
        while (end < out.size() && hexDigit(out[end])) ++end;
        if (end - i >= kSecretHexRun) {
            out.replace(i, end - i, "[redacted]");
            i += 10;
        } else {
            i = end;
        }
    }
    return out;
}

// The context as "key: value" lines, into the buffer the crash handler isn't reading.
void publishContextLocked(const State& s) {
    const int next = 1 - g_contextActive.load(std::memory_order_relaxed);
    size_t length = 0;
    for (const auto& [key, value] : s.context) {
        const std::string line = key + ": " + value + "\n";
        if (length + line.size() > kContextTextBytes) break;
        std::memcpy(g_contextText[next] + length, line.data(), line.size());
        length += line.size();
    }
    g_contextLength[next].store(length, std::memory_order_relaxed);
    g_contextActive.store(next, std::memory_order_release);
}

void ringAppend(std::string_view line) {
    uint64_t written = g_ringWritten.load(std::memory_order_relaxed);
    // Keep only the tail of an enormous line.
    if (line.size() > kRingBytes) line = line.substr(line.size() - kRingBytes);
    for (size_t done = 0; done < line.size();) {
        const size_t at = size_t(written % kRingBytes);
        const size_t n = std::min(line.size() - done, kRingBytes - at);
        std::memcpy(g_ring + at, line.data() + done, n);
        done += n;
        written += n;
    }
    g_ringWritten.store(written, std::memory_order_release);
}

void stopFileLocked(State& s, std::string reason) {
    if (!s.file) return;
    s.fileStatus.stopped = true;
    s.fileStatus.stopReason = std::move(reason);
    std::fclose(s.file);
    s.file = nullptr;
}

void writeFileLocked(State& s, std::string_view line) {
    if (!s.file) return;
    if (s.fileStatus.bytes + line.size() > s.fileOptions.maxBytes) {
        const char note[] = "[the log reached its size limit; later lines are only in memory]\n";
        std::fwrite(note, 1, sizeof(note) - 1, s.file);
        stopFileLocked(s, "size limit");
        return;
    }
    errno = 0;
    const size_t wrote = std::fwrite(line.data(), 1, line.size(), s.file);
    if (wrote != line.size() || std::fflush(s.file) != 0) {
        const int code = errno;
        stopFileLocked(s, code ? std::generic_category().message(code) : std::string("write failed"));
        return;
    }
    s.fileStatus.bytes += line.size();
}

void onLog(const LogCategory& category, LogLevel level, std::string_view message) {
    State& s = state();
    std::string line;
    {
        std::lock_guard lock(s.contextMutex);
        line = redactLocked(s, message);
    }
    std::string text = std::string(category.name) + ": ";
    if (level >= LogLevel::Warning) text += std::string(logLevelName(level)) + ": ";
    text += line;
    text += '\n';
    {
        std::lock_guard lock(s.ringMutex);
        ringAppend(text);
        writeFileLocked(s, text);
    }
    if (level == LogLevel::Error) setContext("last_error", std::string(category.name) + ": " + line);
}

std::string utcNow() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return text;
}

std::string markerText(const State& s) {
    std::string text = "shockwave_session 1\nstarted " + s.startedAt + "\nprogram " + s.program + "\npid " +
                       std::to_string(processId()) + "\n";
    for (const auto& [key, value] : s.context) text += "context " + key + " " + value + "\n";
    return text;
}

void writeMarkerLocked(const State& s) {
    if (s.markerPath.empty()) return;
    const fs::path temporary = s.markerPath.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << markerText(s);
        if (!out) return;
    }
    std::error_code ec;
    fs::rename(temporary, s.markerPath, ec);
    if (ec) fs::remove(temporary, ec);
}

}   // namespace

std::string utcTimestamp() { return utcNow(); }

// ---- Recording ------------------------------------------------------------------------------

void startRecording() {
    State& s = state();
    {
        std::lock_guard lock(s.contextMutex);
        if (s.sink) return;
            // The home folder, so paths in logs don't carry the user's name.
#if defined(_WIN32)
        const auto home = environment("USERPROFILE");
#else
        const auto home = environment("HOME");
#endif
        if (home && home->size() > 1) {
            std::string h = *home;
            while (h.size() > 1 && (h.back() == '/' || h.back() == '\\')) h.pop_back();
            s.redactions.emplace_back(h, "~");
#if defined(_WIN32)
            std::string forward = h;
            std::replace(forward.begin(), forward.end(), '\\', '/');
            if (forward != h) s.redactions.emplace_back(forward, "~");
#endif
        }
    }
    const int id = addLogSink(onLog);
    std::lock_guard lock(s.contextMutex);
    s.sink = id;
}

void stopRecording() {
    State& s = state();
    int id = 0;
    {
        std::lock_guard lock(s.contextMutex);
        id = std::exchange(s.sink, 0);
    }
    if (id) removeLogSink(id);
}

bool recording() {
    State& s = state();
    std::lock_guard lock(s.contextMutex);
    return s.sink != 0;
}

std::vector<std::string> recentLog() {
    State& s = state();
    std::string text;
    {
        std::lock_guard lock(s.ringMutex);
        const uint64_t written = g_ringWritten.load(std::memory_order_acquire);
        if (written <= kRingBytes) {
            text.assign(g_ring, size_t(written));
        } else {
            const size_t at = size_t(written % kRingBytes);
            text.assign(g_ring + at, kRingBytes - at);
            text.append(g_ring, at);
        }
    }
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) lines.push_back(std::move(line));
    return lines;
}

void setContext(std::string_view key, std::string_view value) {
    State& s = state();
    std::lock_guard lock(s.contextMutex);
    std::string clean = redactLocked(s, value);
    std::replace(clean.begin(), clean.end(), '\n', ' ');
    std::replace(clean.begin(), clean.end(), '\r', ' ');
    if (clean.size() > kMaxContextValueBytes) clean.resize(kMaxContextValueBytes);
    if (clean.empty()) {
        if (auto it = s.context.find(key); it != s.context.end()) s.context.erase(it);
    } else if (auto it = s.context.find(key); it != s.context.end()) {
        it->second = std::move(clean);
    } else if (s.context.size() < kMaxContextKeys) {
        s.context.emplace(std::string(key), std::move(clean));
    }
    publishContextLocked(s);
}

std::string contextValue(std::string_view key) {
    State& s = state();
    std::lock_guard lock(s.contextMutex);
    const auto it = s.context.find(key);
    return it == s.context.end() ? std::string() : it->second;
}

std::vector<std::pair<std::string, std::string>> context() {
    State& s = state();
    std::lock_guard lock(s.contextMutex);
    return {s.context.begin(), s.context.end()};
}

void addRedaction(std::string_view text, std::string_view replacement) {
    if (text.empty()) return;
    State& s = state();
    std::lock_guard lock(s.contextMutex);
    for (const auto& [from, to] : s.redactions)
        if (from == text) return;
    s.redactions.emplace_back(std::string(text), std::string(replacement));
    // Longest first, so a longer path wins over a folder above it.
    std::stable_sort(s.redactions.begin(), s.redactions.end(), [](const auto& a, const auto& b) {
        return a.first.size() > b.first.size();
    });
}

std::string redact(std::string_view text) {
    State& s = state();
    std::lock_guard lock(s.contextMutex);
    return redactLocked(s, text);
}

// ---- Log files ------------------------------------------------------------------------------

bool openLogFile(const LogFileOptions& options, std::string& error) {
    closeLogFile();
    std::error_code ec;
    fs::path file = options.exactFile;
    if (file.empty()) {
        if (options.folder.empty()) {
            error = "no log folder";
            return false;
        }
        fs::create_directories(options.folder, ec);
        if (ec) {
            error = "couldn't make " + options.folder.string() + ": " + ec.message();
            return false;
        }
        // <name>.log -> <name>.1.log -> ... ; the oldest goes.
        const auto numbered = [&](int n) {
            return options.folder / (options.name + (n ? "." + std::to_string(n) : std::string()) + ".log");
        };
        const int keep = std::max(0, options.keep);
        fs::remove(numbered(keep), ec);
        for (int n = keep - 1; n >= 0; --n)
            if (fs::exists(numbered(n), ec)) fs::rename(numbered(n), numbered(n + 1), ec);
        if (keep == 0) fs::remove(numbered(0), ec);
        file = numbered(0);
    }
    std::FILE* handle = nullptr;
#if defined(_WIN32)
    handle = _wfopen(file.c_str(), L"wb");
#else
    handle = std::fopen(file.c_str(), "wb");
#endif
    if (!handle) {
        error = "couldn't open " + file.string() + ": " + std::generic_category().message(errno);
        return false;
    }
    State& s = state();
    std::lock_guard lock(s.ringMutex);
    s.file = handle;
    s.filePath = file;
    s.fileOptions = options;
    s.fileStatus = {};
    s.fileStatus.open = true;
    return true;
}

void closeLogFile() {
    State& s = state();
    std::lock_guard lock(s.ringMutex);
    if (s.file) std::fclose(s.file);
    s.file = nullptr;
    s.filePath.clear();
    s.fileStatus.open = false;
}

fs::path logFilePath() {
    State& s = state();
    std::lock_guard lock(s.ringMutex);
    return s.filePath;
}

LogFileStatus logFileStatus() {
    State& s = state();
    std::lock_guard lock(s.ringMutex);
    LogFileStatus status = s.fileStatus;
    status.open = s.file != nullptr;
    return status;
}

// ---- Session marker -------------------------------------------------------------------------

PreviousSession inspectSession(const fs::path& folder, const fs::path& crashes, std::string_view program) {
    PreviousSession previous;
    std::error_code ec;
    const fs::path marker = folder / (std::string(program) + ".session");
    std::ifstream in(marker, std::ios::binary);
    if (!in) return previous;
    const fs::file_time_type markerTime = fs::last_write_time(marker, ec);
    previous.found = true;
    for (std::string line; std::getline(in, line);) {
        const size_t space = line.find(' ');
        const std::string key = line.substr(0, space);
        const std::string rest = space == std::string::npos ? std::string() : line.substr(space + 1);
        if (key == "started")
            previous.startedAt = rest;
        else if (key == "program")
            previous.program = rest;
        else if (key == "pid")
            previous.pid = std::strtoll(rest.c_str(), nullptr, 10);
        else if (key == "context") {
            const size_t gap = rest.find(' ');
            if (gap != std::string::npos) previous.context.emplace_back(rest.substr(0, gap), rest.substr(gap + 1));
        }
    }
    previous.stillRunning = processAlive(previous.pid);
    // That run's report: the newest one of this program's, written no earlier than a day
    // before the marker's last rewrite (the marker is rewritten as levels change).
    if (!previous.stillRunning) {
        const std::string prefix = previous.program.empty() ? std::string(program) : previous.program;
        for (const fs::path& report : crashReports(crashes)) {
            if (report.filename().string().rfind(prefix + "-", 0) != 0) continue;
            const auto when = fs::last_write_time(report, ec);
            if (!ec && when + std::chrono::hours(24) >= markerTime) previous.crashReport = report;
            break;
        }
    }
    return previous;
}

bool beginSession(
    const fs::path& folder, const fs::path& crashes, std::string_view program, PreviousSession& previous,
    std::string& error) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        error = "couldn't make " + folder.string() + ": " + ec.message();
        return false;
    }
    previous = inspectSession(folder, crashes, program);
    const fs::path marker = folder / (std::string(program) + ".session");
    State& s = state();
    std::lock_guard lock(s.contextMutex);
    s.program = std::string(program);
    s.startedAt = utcNow();
    // Another copy still running keeps its marker; this one gets its own.
    s.markerPath =
        previous.stillRunning ? folder / (s.program + "." + std::to_string(processId()) + ".session") : marker;
    s.previous = previous;
    writeMarkerLocked(s);
    if (!fs::exists(s.markerPath, ec)) {
        error = "couldn't write " + s.markerPath.string();
        s.markerPath.clear();
        return false;
    }
    return true;
}

void updateSession() {
    State& s = state();
    std::lock_guard lock(s.contextMutex);
    writeMarkerLocked(s);
}

void endSession() {
    State& s = state();
    std::lock_guard lock(s.contextMutex);
    if (s.markerPath.empty()) return;
    std::error_code ec;
    fs::remove(s.markerPath, ec);
    s.markerPath.clear();
}

void leaveSession() {
    State& s = state();
    std::lock_guard lock(s.contextMutex);
    writeMarkerLocked(s);
    s.markerPath.clear();
}

const PreviousSession& previousSession() { return state().previous; }

std::vector<fs::path> crashReports(const fs::path& folder) {
    std::vector<std::pair<fs::file_time_type, fs::path>> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.size() > 10 && name.ends_with(".crash.txt") && entry.is_regular_file(ec))
            found.emplace_back(fs::last_write_time(entry.path(), ec), entry.path());
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second > b.second;
    });
    std::vector<fs::path> reports;
    for (auto& [when, path] : found) reports.push_back(std::move(path));
    return reports;
}

}   // namespace shockwave::diagnostics
