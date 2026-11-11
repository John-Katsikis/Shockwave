// Crash capture (Core/Diagnostics.h). A fatal fault writes a report to the Crashes folder:
//
//   shockwave_crash 1
//   program player
//   signal SIGSEGV (11)
//   fault_address 0x0
//   time_unix 1762901652
//   [context]        the session context, as the engine last set it
//   [frames]         return addresses, innermost first ("0x55d0c1a2b3c4", Windows adds
//                    "+<offset> <module>")
//   [modules]        Linux: /proc/self/maps, to find each address's binary and offset
//   [symbols]        macOS: what backtrace_symbols_fd says about each frame
//   [log]            the recent-log ring, oldest first
//
// POSIX: the handler runs on its own stack and calls only async-signal-safe functions
// (open, write, close, read, sigaction, raise) on memory prepared at install time, plus
// backtrace(), whose library is loaded at install so it doesn't allocate in the handler.
// It never formats with the C++ library, takes the log's lock, allocates, or touches
// Vulkan. Then the default action runs (a core dump where the system makes them).
// Windows: an unhandled-exception filter writes the report and a minidump (dbghelp's
// MiniDumpWriteDump, the system's supported way), and abort() is caught as SIGABRT.
#include "DiagnosticsShared.h"

#include <atomic>
#include <ctime>
#include <cstring>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// dbghelp after windows.h.
#include <dbghelp.h>
#include <csignal>
#include <cstdlib>
#else
#include <csignal>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace shockwave::diagnostics {

namespace fs = std::filesystem;
using namespace detail;

namespace {

constexpr int kMaxFrames = 64;
constexpr size_t kPathBytes = 4096;

char g_reportPath[kPathBytes];
char g_folderPath[kPathBytes];
char g_program[64];
std::atomic<bool> g_installed{false};
std::atomic<bool> g_crashing{false};
CrashOptions g_options;

// ---- Text without the C library's formatting --------------------------------------------

struct Out {
    char buffer[512];
    size_t used = 0;
    void put(const char* text, size_t n) {
        for (size_t i = 0; i < n && used < sizeof(buffer); ++i) buffer[used++] = text[i];
    }
    void put(const char* text) { put(text, std::strlen(text)); }
    void hex(uint64_t value) {
        char digits[19] = "0x";
        char tmp[16];
        int n = 0;
        do {
            tmp[n++] = "0123456789abcdef"[value & 15];
            value >>= 4;
        } while (value);
        size_t at = 2;
        while (n) digits[at++] = tmp[--n];
        put(digits, at);
    }
    void dec(int64_t value) {
        char tmp[24];
        int n = 0;
        const bool negative = value < 0;
        uint64_t v = negative ? uint64_t(-value) : uint64_t(value);
        do {
            tmp[n++] = char('0' + v % 10);
            v /= 10;
        } while (v);
        if (negative) put("-");
        while (n) put(&tmp[--n], 1);
    }
};

const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
#if !defined(_WIN32)
    case SIGBUS: return "SIGBUS";
    case SIGTRAP: return "SIGTRAP";
#endif
    default: return "signal";
    }
}

void copyPath(char* to, const std::string& from) {
    const size_t n = std::min(from.size(), kPathBytes - 1);
    std::memcpy(to, from.data(), n);
    to[n] = 0;
}

// The report's file name: <program>-<UTC time>-<pid>.crash.txt.
std::string reportName(const std::string& program, int64_t pid) {
    std::string stamp = utcTimestamp();   // 2026-11-11T22:54:12Z
    std::string compact;
    for (char c : stamp)
        if (c != '-' && c != ':' && c != 'Z') compact += c == 'T' ? '-' : c;
    return program + "-" + compact + "-" + std::to_string(pid) + ".crash.txt";
}

#if !defined(_WIN32)

int g_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};
struct sigaction g_previous[sizeof(g_signals) / sizeof(g_signals[0])];
alignas(16) char g_altStack[64 * 1024];

void writeAll(int fd, const char* data, size_t n) {
    while (n) {
        const ssize_t wrote = write(fd, data, n);
        if (wrote <= 0) return;
        data += wrote;
        n -= size_t(wrote);
    }
}
void writeText(int fd, const char* text) { writeAll(fd, text, std::strlen(text)); }

void writeReport(int fd, int sig, void* faultAddress) {
    Out header;
    header.put("shockwave_crash 1\nprogram ");
    header.put(g_program);
    header.put("\nsignal ");
    header.put(signalName(sig));
    header.put(" (");
    header.dec(sig);
    header.put(")\nfault_address ");
    header.hex(uint64_t(reinterpret_cast<uintptr_t>(faultAddress)));
    header.put("\ntime_unix ");
    header.dec(int64_t(time(nullptr)));
    header.put("\n[context]\n");
    writeAll(fd, header.buffer, header.used);
    const int active = g_contextActive.load(std::memory_order_acquire);
    writeAll(fd, g_contextText[active], g_contextLength[active].load(std::memory_order_relaxed));

    writeText(fd, "[frames]\n");
    void* frames[kMaxFrames];
    const int count = backtrace(frames, kMaxFrames);
    for (int i = 0; i < count; ++i) {
        Out line;
        line.hex(uint64_t(reinterpret_cast<uintptr_t>(frames[i])));
        line.put("\n");
        writeAll(fd, line.buffer, line.used);
    }
#if defined(__linux__)
    writeText(fd, "[modules]\n");
    const int maps = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (maps >= 0) {
        char chunk[4096];
        for (ssize_t n; (n = read(maps, chunk, sizeof(chunk))) > 0;) writeAll(fd, chunk, size_t(n));
        close(maps);
    }
#else
    writeText(fd, "[symbols]\n");
    backtrace_symbols_fd(frames, count, fd);
#endif
    writeText(fd, "[log]\n");
    const uint64_t written = g_ringWritten.load(std::memory_order_acquire);
    if (written <= kRingBytes) {
        writeAll(fd, g_ring, size_t(written));
    } else {
        const size_t at = size_t(written % kRingBytes);
        writeAll(fd, g_ring + at, kRingBytes - at);
        writeAll(fd, g_ring, at);
    }
    writeText(fd, "\n[end]\n");
}

// Makes the Crashes folder and any missing parents, with mkdir only.
void makeFolders() {
    char path[kPathBytes];
    std::memcpy(path, g_folderPath, kPathBytes);
    for (size_t i = 1; path[i]; ++i)
        if (path[i] == '/') {
            path[i] = 0;
            mkdir(path, 0755);
            path[i] = '/';
        }
    mkdir(path, 0755);
}

void onSignal(int sig, siginfo_t* info, void*) {
    if (!g_crashing.exchange(true)) {
        makeFolders();
        const int fd = open(g_reportPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) {
            writeReport(fd, sig, info ? info->si_addr : nullptr);
            close(fd);
        }
        writeText(2, g_program);
        writeText(2, " crashed (");
        writeText(2, signalName(sig));
        writeText(2, "). A report was written to ");
        writeText(2, g_reportPath);
        writeText(2, "\n");
    }
    // The default action: the process ends the way it would have without us.
    struct sigaction fallback {};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    sigaction(sig, &fallback, nullptr);
    raise(sig);
}

bool installPlatform(std::string& error) {
    // Loads the unwinder now, so the first backtrace() in the handler doesn't allocate.
    void* warm[2];
    backtrace(warm, 2);
    stack_t stack{};
    stack.ss_sp = g_altStack;
    stack.ss_size = sizeof(g_altStack);
    if (sigaltstack(&stack, nullptr) != 0) {
        error = "sigaltstack failed";
        return false;
    }
    for (size_t i = 0; i < sizeof(g_signals) / sizeof(g_signals[0]); ++i) {
        struct sigaction action {};
        action.sa_sigaction = onSignal;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        if (sigaction(g_signals[i], &action, &g_previous[i]) != 0) {
            error = "sigaction failed";
            return false;
        }
    }
    return true;
}

void uninstallPlatform() {
    for (size_t i = 0; i < sizeof(g_signals) / sizeof(g_signals[0]); ++i)
        sigaction(g_signals[i], &g_previous[i], nullptr);
}

int64_t currentPid() { return int64_t(getpid()); }

#else   // Windows

wchar_t g_reportPathWide[kPathBytes];
wchar_t g_folderPathWide[kPathBytes];
wchar_t g_dumpPathWide[kPathBytes];
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
void (*g_previousAbort)(int) = nullptr;

void writeAll(HANDLE file, const char* data, size_t n) {
    DWORD wrote = 0;
    while (n) {
        if (!WriteFile(file, data, DWORD(std::min<size_t>(n, 1 << 20)), &wrote, nullptr) || wrote == 0) return;
        data += wrote;
        n -= wrote;
    }
}
void writeText(HANDLE file, const char* text) { writeAll(file, text, std::strlen(text)); }

void makeFolders() {
    wchar_t path[kPathBytes];
    std::memcpy(path, g_folderPathWide, sizeof(path));
    for (size_t i = 3; path[i]; ++i)
        if (path[i] == L'\\' || path[i] == L'/') {
            const wchar_t keep = path[i];
            path[i] = 0;
            CreateDirectoryW(path, nullptr);
            path[i] = keep;
        }
    CreateDirectoryW(path, nullptr);
}

void writeReport(const char* what, uint64_t code, uint64_t address, EXCEPTION_POINTERS* pointers) {
    makeFolders();
    HANDLE file =
        CreateFileW(g_reportPathWide, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    Out header;
    header.put("shockwave_crash 1\nprogram ");
    header.put(g_program);
    header.put("\nsignal ");
    header.put(what);
    header.put(" (");
    header.hex(code);
    header.put(")\nfault_address ");
    header.hex(address);
    header.put("\ntime_unix ");
    header.dec(int64_t(time(nullptr)));
    header.put("\n[context]\n");
    writeAll(file, header.buffer, header.used);
    const int active = g_contextActive.load(std::memory_order_acquire);
    writeAll(file, g_contextText[active], g_contextLength[active].load(std::memory_order_relaxed));
    writeText(file, "[frames]\n");
    void* frames[kMaxFrames];
    const USHORT count = RtlCaptureStackBackTrace(0, kMaxFrames, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        Out line;
        line.hex(uint64_t(reinterpret_cast<uintptr_t>(frames[i])));
        HMODULE module = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCSTR>(frames[i]), &module) &&
            module) {
            char name[MAX_PATH];
            const DWORD length = GetModuleFileNameA(module, name, MAX_PATH);
            line.put(" +");
            line.hex(uint64_t(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(module)));
            line.put(" ");
            line.put(name, length);
        }
        line.put("\n");
        writeAll(file, line.buffer, line.used);
    }
    writeText(file, "[log]\n");
    const uint64_t written = g_ringWritten.load(std::memory_order_acquire);
    if (written <= kRingBytes) {
        writeAll(file, g_ring, size_t(written));
    } else {
        const size_t at = size_t(written % kRingBytes);
        writeAll(file, g_ring + at, kRingBytes - at);
        writeAll(file, g_ring, at);
    }
    writeText(file, "\n[end]\n");
    CloseHandle(file);

    if (g_options.minidump) {
        HANDLE dump =
            CreateFileW(g_dumpPathWide, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dump != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION exception{};
            exception.ThreadId = GetCurrentThreadId();
            exception.ExceptionPointers = pointers;
            exception.ClientPointers = FALSE;
            MiniDumpWriteDump(
                GetCurrentProcess(), GetCurrentProcessId(), dump, MiniDumpNormal, pointers ? &exception : nullptr,
                nullptr, nullptr);
            CloseHandle(dump);
        }
    }
}

LONG WINAPI onException(EXCEPTION_POINTERS* pointers) {
    if (!g_crashing.exchange(true)) {
        const auto* record = pointers ? pointers->ExceptionRecord : nullptr;
        writeReport(
            "exception", record ? record->ExceptionCode : 0,
            record ? uint64_t(reinterpret_cast<uintptr_t>(record->ExceptionAddress)) : 0, pointers);
    }
    return g_previousFilter ? g_previousFilter(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

void onAbort(int sig) {
    if (!g_crashing.exchange(true)) writeReport("SIGABRT", uint64_t(sig), 0, nullptr);
    std::signal(SIGABRT, SIG_DFL);
    std::raise(SIGABRT);
}

bool installPlatform(std::string&) {
    g_previousFilter = SetUnhandledExceptionFilter(onException);
    g_previousAbort = std::signal(SIGABRT, onAbort);
    return true;
}

void uninstallPlatform() {
    SetUnhandledExceptionFilter(g_previousFilter);
    std::signal(SIGABRT, g_previousAbort ? g_previousAbort : SIG_DFL);
}

int64_t currentPid() { return int64_t(GetCurrentProcessId()); }

#endif

}   // namespace

bool crashCaptureSupported() {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    return true;
#else
    return false;
#endif
}

bool installCrashHandler(const CrashOptions& options, std::string& error) {
    if (!crashCaptureSupported()) {
        error = "this platform has no crash capture";
        return false;
    }
    if (options.folder.empty()) {
        error = "no crash folder";
        return false;
    }
    // The folder is made only if a crash happens (nothing is created until it's needed).
    g_options = options;
    const std::string program = options.program.empty() ? std::string("program") : options.program;
    const size_t n = std::min(program.size(), sizeof(g_program) - 1);
    std::memcpy(g_program, program.data(), n);
    g_program[n] = 0;
    const fs::path report = fs::absolute(options.folder) / reportName(program, currentPid());
    copyPath(g_reportPath, report.string());
    copyPath(g_folderPath, fs::absolute(options.folder).string());
#if defined(_WIN32)
    wcsncpy_s(g_folderPathWide, fs::absolute(options.folder).wstring().c_str(), _TRUNCATE);
    const std::wstring wide = report.wstring();
    const std::wstring dump = fs::path(report).replace_extension("").replace_extension(".dmp").wstring();
    wcsncpy_s(g_reportPathWide, wide.c_str(), _TRUNCATE);
    wcsncpy_s(g_dumpPathWide, dump.c_str(), _TRUNCATE);
#endif
    if (g_installed.load()) return true;
    if (!installPlatform(error)) return false;
    g_installed = true;
    return true;
}

void uninstallCrashHandler() {
    if (!g_installed.exchange(false)) return;
    uninstallPlatform();
}

bool crashHandlerInstalled() { return g_installed.load(); }

fs::path pendingCrashReport() { return g_installed.load() ? fs::path(g_reportPath) : fs::path(); }

}   // namespace shockwave::diagnostics
