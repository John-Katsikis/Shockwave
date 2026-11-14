// Child processes without a shell (Core/Process.h).
#include <Core/Process.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwchar>
#include <cwctype>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#if defined(__APPLE__)
#include <crt_externs.h>
#include <mach-o/dyld.h>
#define environ (*_NSGetEnviron())
#else
extern char** environ;
#endif
#endif

namespace shockwave {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

// Splits output into lines as it arrives. A line longer than 1 MiB is passed on in pieces
// so a child that never writes a newline can't grow it without end.
class LineSplitter {
public:
    explicit LineSplitter(const std::function<void(std::string_view)>& onLine) : m_onLine(onLine) {}
    void add(const char* data, size_t size) {
        if (!m_onLine) return;
        for (size_t i = 0; i < size; ++i) {
            if (data[i] == '\n') {
                emit();
            } else {
                m_text += data[i];
                if (m_text.size() >= (1u << 20)) emit();
            }
        }
    }
    void finish() {
        if (!m_text.empty()) emit();
    }

private:
    void emit() {
        while (!m_text.empty() && m_text.back() == '\r') m_text.pop_back();
        m_onLine(m_text);
        m_text.clear();
    }
    const std::function<void(std::string_view)>& m_onLine;
    std::string m_text;
};

std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool cancelled(const ProcessOptions& options) { return options.cancel && options.cancel->load(); }

bool hasSeparator(const std::string& text) {
#if defined(_WIN32)
    return text.find_first_of("/\\:") != std::string::npos;
#else
    return text.find('/') != std::string::npos;
#endif
}

// The program to start: a name with no folder is looked up on PATH; a relative path is
// taken from this process's folder (before any working directory change).
bool resolveExecutable(const ProcessOptions& options, fs::path& out, std::string& error) {
    const std::string given = utf8(options.executable);
    if (given.empty()) {
        error = "no program to run";
        return false;
    }
    std::error_code ec;
    if (!hasSeparator(given)) {
        out = findExecutable(given);
        if (out.empty()) {
            error = given + " wasn't found on PATH";
            return false;
        }
        return true;
    }
    out = fs::absolute(options.executable, ec);
    if (ec || !fs::is_regular_file(out, ec)) {
        error = given + " doesn't exist";
        return false;
    }
    return true;
}

std::string quoteForLog(const std::string& text) {
    if (!text.empty() && text.find_first_of(" \t\"'") == std::string::npos) return text;
    std::string out = "\"";
    for (const char c : text) out += c == '"' ? std::string("\\\"") : std::string(1, c);
    return out + "\"";
}

#if defined(_WIN32)

std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size);
    return out;
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string lastError(const char* what) {
    const DWORD code = GetLastError();
    wchar_t* text = nullptr;
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
        reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string message = text ? narrow(text) : std::string();
    if (text) LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' '))
        message.pop_back();
    return std::string(what) + " failed (" + std::to_string(code) + (message.empty() ? "" : ": " + message) + ")";
}

std::wstring upper(std::wstring text) {
    for (wchar_t& c : text) c = wchar_t(towupper(c));
    return text;
}

// The environment block CreateProcessW takes: NAME=value\0 ... \0, sorted by name without
// regard to case, as Windows expects.
std::wstring environmentBlock(const ProcessEnvironment& env) {
    std::vector<std::pair<std::wstring, std::wstring>> vars;   // name, whole entry
    if (env.inherit) {
        if (wchar_t* strings = GetEnvironmentStringsW()) {
            for (const wchar_t* p = strings; *p; p += wcslen(p) + 1) {
                const std::wstring entry = p;
                const size_t eq = entry.find(L'=', 1);   // "=C:=C:\\" entries start with '='
                vars.emplace_back(entry.substr(0, eq), entry);
            }
            FreeEnvironmentStringsW(strings);
        }
    }
    auto erase = [&](const std::wstring& name) {
        const std::wstring key = upper(name);
        vars.erase(
            std::remove_if(vars.begin(), vars.end(), [&](const auto& v) { return upper(v.first) == key; }), vars.end());
    };
    for (const std::string& name : env.unset) erase(widen(name));
    for (const auto& [name, value] : env.set) {
        erase(widen(name));
        vars.emplace_back(widen(name), widen(name) + L"=" + widen(value));
    }
    std::sort(vars.begin(), vars.end(), [](const auto& a, const auto& b) { return upper(a.first) < upper(b.first); });
    std::wstring block;
    for (const auto& v : vars) {
        block += v.second;
        block += L'\0';
    }
    if (vars.empty()) block += L'\0';
    block += L'\0';
    return block;
}

struct Started {
    PROCESS_INFORMATION info{};
    HANDLE job = nullptr;
    HANDLE output = nullptr;   // our end of the pipe; null when detached
};

// Starts the child suspended in a job of its own, with stdout and stderr on one pipe and
// stdin on NUL; only those handles are inherited.
bool startWindows(
    const ProcessOptions& options, const std::wstring& application, std::wstring commandLine, bool detached,
    Started& out, std::string& error) {
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr, nul = nullptr;
    if (!detached) {
        if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0)) {
            error = lastError("CreatePipe");
            return false;
        }
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
        nul =
            CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    }
    auto closeAll = [&] {
        if (readEnd) CloseHandle(readEnd);
        if (writeEnd) CloseHandle(writeEnd);
        if (nul && nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    };

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    std::vector<char> attributes;
    HANDLE handles[2] = {writeEnd, nul};
    if (!detached) {
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = nul;
        startup.StartupInfo.hStdOutput = writeEnd;
        startup.StartupInfo.hStdError = writeEnd;
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attributes.resize(size);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size) ||
            !UpdateProcThreadAttribute(
                startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                (nul && nul != INVALID_HANDLE_VALUE ? 2 : 1) * sizeof(HANDLE), nullptr, nullptr)) {
            error = lastError("UpdateProcThreadAttribute");
            closeAll();
            return false;
        }
    }
    std::wstring env = environmentBlock(options.environment);
    const std::wstring folder = options.workingDirectory.empty() ? std::wstring() : options.workingDirectory.wstring();
    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    if (detached)
        flags |= DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    else
        flags |= CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
    BOOL created = FALSE;
    for (int attempt = 0; attempt < 2 && !created; ++attempt) {
        // A detached program first tries to leave our job (an IDE's, a CI runner's), so it
        // isn't ended with us; not every job allows that.
        const DWORD extra = detached && attempt == 0 ? CREATE_BREAKAWAY_FROM_JOB : 0;
        created = CreateProcessW(
            application.c_str(), commandLine.data(), nullptr, nullptr, detached ? FALSE : TRUE, flags | extra,
            env.data(), folder.empty() ? nullptr : folder.c_str(), &startup.StartupInfo, &out.info);
        if (!detached) break;
    }
    if (!attributes.empty()) DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!created) {
        error = lastError("CreateProcess");
        closeAll();
        return false;
    }
    if (writeEnd) CloseHandle(writeEnd);
    if (nul && nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    out.output = readEnd;
    if (!detached) {
        out.job = CreateJobObjectW(nullptr, nullptr);
        if (out.job && !AssignProcessToJobObject(out.job, out.info.hProcess)) {
            CloseHandle(out.job);   // runs without one: only the child itself can be ended
            out.job = nullptr;
        }
        ResumeThread(out.info.hThread);
    }
    return true;
}

ProcessResult runWindows(const ProcessOptions& options, const std::wstring& application, std::wstring commandLine) {
    ProcessResult result;
    Started child;
    if (!startWindows(options, application, std::move(commandLine), false, child, result.error)) return result;
    result.started = true;
    LineSplitter lines(options.onLine);
    const auto start = Clock::now();
    auto endChild = [&] {
        if (child.job)
            TerminateJobObject(child.job, 1);
        else
            TerminateProcess(child.info.hProcess, 1);
    };
    bool exited = false, eof = false;
    Clock::time_point exitedAt{};
    char buffer[4096];
    while (!eof) {
        DWORD available = 0;
        if (!PeekNamedPipe(child.output, nullptr, 0, nullptr, &available, nullptr)) {
            eof = true;   // every writer is gone
            break;
        }
        if (available) {
            DWORD got = 0;
            if (!ReadFile(child.output, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) || !got) {
                eof = true;
                break;
            }
            lines.add(buffer, got);
            continue;
        }
        if (!exited && WaitForSingleObject(child.info.hProcess, 20) == WAIT_OBJECT_0) {
            exited = true;
            exitedAt = Clock::now();
        } else if (exited) {
            // Something it started still holds the pipe: stop reading after a second.
            if (Clock::now() - exitedAt > std::chrono::seconds(1)) break;
            Sleep(20);
        }
        if (!exited) {
            const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
            if (options.timeoutSeconds > 0 && elapsed > options.timeoutSeconds) result.timedOut = true;
            if (cancelled(options)) result.cancelled = true;
            if (result.timedOut || result.cancelled) {
                endChild();
                WaitForSingleObject(child.info.hProcess, 5000);
                exited = true;
                exitedAt = Clock::now();
            }
        }
    }
    lines.finish();
    WaitForSingleObject(child.info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(child.info.hProcess, &code);
    result.exitCode = int(code);
    if (result.timedOut || result.cancelled) endChild();   // anything it started, too
    CloseHandle(child.output);
    CloseHandle(child.info.hThread);
    CloseHandle(child.info.hProcess);
    if (child.job) CloseHandle(child.job);
    return result;
}

#else   // POSIX

// Everything the child needs, made before fork: after it, only async-signal-safe calls.
struct Prepared {
    std::string program;
    std::vector<std::string> argvText, envText;
    std::vector<char*> argv, envp;
    std::string folder;
};

void prepare(const ProcessOptions& options, const fs::path& program, Prepared& p) {
    p.program = program.string();
    p.argvText.push_back(options.executable.string());
    for (const std::string& argument : options.arguments) p.argvText.push_back(argument);
    auto unsetHere = [&](std::string_view name) {
        for (const std::string& n : options.environment.unset)
            if (n == name) return true;
        for (const auto& [n, value] : options.environment.set)
            if (n == name) return true;
        return false;
    };
    if (options.environment.inherit) {
        for (char** e = environ; e && *e; ++e) {
            const std::string_view entry = *e;
            const size_t eq = entry.find('=');
            if (unsetHere(entry.substr(0, eq))) continue;
            p.envText.emplace_back(entry);
        }
    }
    for (const auto& [name, value] : options.environment.set) p.envText.push_back(name + "=" + value);
    for (std::string& text : p.argvText) p.argv.push_back(text.data());
    p.argv.push_back(nullptr);
    for (std::string& text : p.envText) p.envp.push_back(text.data());
    p.envp.push_back(nullptr);
    p.folder = options.workingDirectory.string();
}

bool makePipe(int fds[2]) {
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
}

// In the child, after fork: becomes the leader of a new process group (so a timeout ends
// everything it starts), wires its output, changes folder and execs. A failure is written
// to `report` as {stage, errno}: 1 folder, 2 exec.
[[noreturn]] void execChild(const Prepared& p, int output, int report, bool newSession) {
    if (newSession) {
        setsid();
    } else {
        setpgid(0, 0);
#if defined(__linux__)
        // A process group doesn't reach a grandchild that made its own (a nested
        // runProcess); this does: the child goes when the thread waiting for it does.
        prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    }
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    for (int s : {SIGPIPE, SIGINT, SIGTERM, SIGCHLD}) sigaction(s, &dfl, nullptr);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    const int nul = open("/dev/null", O_RDWR);
    if (nul >= 0) dup2(nul, 0);
    const int out = output >= 0 ? output : nul;
    if (out >= 0) {
        dup2(out, 1);
        dup2(out, 2);
    }
    int failure[2] = {0, 0};
    if (!p.folder.empty() && chdir(p.folder.c_str()) != 0) {
        failure[0] = 1;
        failure[1] = errno;
    } else {
        execve(p.program.c_str(), p.argv.data(), p.envp.data());
        failure[0] = 2;
        failure[1] = errno;
    }
    ssize_t ignored = write(report, failure, sizeof(failure));
    (void)ignored;
    _exit(127);
}

// Reads the exec report: true when the child is running the program.
bool readReport(int report, const Prepared& p, std::string& error) {
    int failure[2] = {0, 0};
    ssize_t got;
    do {
        got = read(report, failure, sizeof(failure));
    } while (got < 0 && errno == EINTR);
    close(report);
    if (got != ssize_t(sizeof(failure))) return true;   // closed on exec
    error = failure[0] == 1 ? "can't change to " + p.folder + ": " + std::strerror(failure[1])
                            : "can't run " + p.program + ": " + std::strerror(failure[1]);
    return false;
}

void decodeStatus(int status, ProcessResult& result) {
    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.signal = WTERMSIG(status);
        result.exitCode = 128 + result.signal;
    }
}

ProcessResult runPosix(const ProcessOptions& options, const fs::path& program) {
    ProcessResult result;
    Prepared p;
    prepare(options, program, p);
    int output[2], report[2];
    if (!makePipe(output)) {
        result.error = std::string("pipe failed: ") + std::strerror(errno);
        return result;
    }
    if (!makePipe(report)) {
        result.error = std::string("pipe failed: ") + std::strerror(errno);
        close(output[0]);
        close(output[1]);
        return result;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        result.error = std::string("fork failed: ") + std::strerror(errno);
        for (int fd : {output[0], output[1], report[0], report[1]}) close(fd);
        return result;
    }
    if (pid == 0) execChild(p, output[1], report[1], false);
    setpgid(pid, pid);   // also here, so it's a group before either side uses it
    close(output[1]);
    close(report[1]);
    if (!readReport(report[0], p, result.error)) {
        close(output[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        return result;
    }
    result.started = true;
    fcntl(output[0], F_SETFL, fcntl(output[0], F_GETFL) | O_NONBLOCK);

    LineSplitter lines(options.onLine);
    const auto start = Clock::now();
    bool exited = false;
    int status = 0;
    Clock::time_point exitedAt{};
    char buffer[4096];
    auto endGroup = [&] {
        kill(-pid, SIGTERM);
        const auto termAt = Clock::now();
        while (!exited && Clock::now() - termAt < std::chrono::seconds(2)) {
            if (waitpid(pid, &status, WNOHANG) == pid) {
                exited = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        kill(-pid, SIGKILL);   // the child if it's still there, and whatever it started
        if (!exited && waitpid(pid, &status, 0) == pid) exited = true;
        exitedAt = Clock::now();
    };
    for (;;) {
        pollfd fd{output[0], POLLIN, 0};
        const int ready = poll(&fd, 1, 50);
        bool eof = false;
        if (ready > 0) {
            for (;;) {
                const ssize_t got = read(output[0], buffer, sizeof(buffer));
                if (got > 0) {
                    lines.add(buffer, size_t(got));
                    continue;
                }
                if (got == 0) eof = true;
                if (got < 0 && errno == EINTR) continue;
                break;   // EAGAIN: nothing more for now
            }
        }
        if (!exited && waitpid(pid, &status, WNOHANG) == pid) {
            exited = true;
            exitedAt = Clock::now();
        }
        if (eof) break;
        if (exited) {
            // Something it started in the background still holds the pipe: stop reading
            // after a second and leave it be.
            if (Clock::now() - exitedAt > std::chrono::seconds(1)) break;
            continue;
        }
        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        if (options.timeoutSeconds > 0 && elapsed > options.timeoutSeconds) result.timedOut = true;
        if (cancelled(options)) result.cancelled = true;
        if (result.timedOut || result.cancelled) endGroup();
    }
    lines.finish();
    close(output[0]);
    if (!exited) waitpid(pid, &status, 0);
    decodeStatus(status, result);
    return result;
}

#endif

ProcessResult runResolved(const ProcessOptions& options, const fs::path& program, const std::string* rawCommandLine) {
#if defined(_WIN32)
    std::vector<std::string> argv{utf8(options.executable)};
    for (const std::string& a : options.arguments) argv.push_back(a);
    return runWindows(options, program.wstring(), widen(rawCommandLine ? *rawCommandLine : windowsCommandLine(argv)));
#else
    (void)rawCommandLine;
    return runPosix(options, program);
#endif
}

}   // namespace

std::filesystem::path findExecutable(const std::string& name) {
    if (name.empty()) return {};
    std::error_code ec;
#if defined(_WIN32)
    std::vector<std::wstring> extensions{L""};
    if (fs::path(widen(name)).extension().empty()) {
        extensions.clear();
        wchar_t pathext[1024];
        const DWORD n = GetEnvironmentVariableW(L"PATHEXT", pathext, 1024);
        std::wstring list = n && n < 1024 ? std::wstring(pathext, n) : std::wstring(L".COM;.EXE;.BAT;.CMD");
        for (size_t at = 0; at <= list.size();) {
            const size_t end = std::min(list.find(L';', at), list.size());
            if (end > at) extensions.push_back(list.substr(at, end - at));
            at = end + 1;
        }
    }
    std::vector<wchar_t> path(32768);
    const DWORD size = GetEnvironmentVariableW(L"PATH", path.data(), DWORD(path.size()));
    const std::wstring dirs(path.data(), size < path.size() ? size : 0);
    for (size_t at = 0; at <= dirs.size();) {
        const size_t end = std::min(dirs.find(L';', at), dirs.size());
        std::wstring dir = dirs.substr(at, end - at);
        at = end + 1;
        if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"') dir = dir.substr(1, dir.size() - 2);
        if (dir.empty()) continue;
        for (const std::wstring& ext : extensions) {
            const fs::path candidate = fs::path(dir) / (widen(name) + ext);
            if (fs::is_regular_file(candidate, ec)) return candidate;
        }
    }
#else
    const char* path = std::getenv("PATH");
    const std::string dirs = path ? path : "/usr/local/bin:/usr/bin:/bin";
    for (size_t at = 0; at <= dirs.size();) {
        const size_t end = std::min(dirs.find(':', at), dirs.size());
        const std::string dir = dirs.substr(at, end - at);
        at = end + 1;
        const fs::path candidate = fs::path(dir.empty() ? "." : dir) / name;
        if (fs::is_regular_file(candidate, ec) && access(candidate.c_str(), X_OK) == 0) return candidate;
    }
#endif
    return {};
}

std::string windowsCommandLine(const std::vector<std::string>& argv) {
    std::string line;
    for (size_t i = 0; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (i) line += ' ';
        if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
            line += arg;
            continue;
        }
        line += '"';
        for (size_t at = 0;; ++at) {
            size_t slashes = 0;
            while (at < arg.size() && arg[at] == '\\') ++at, ++slashes;
            if (at == arg.size()) {
                line.append(slashes * 2, '\\');   // before the closing quote
                break;
            }
            if (arg[at] == '"') {
                line.append(slashes * 2 + 1, '\\');
                line += '"';
            } else {
                line.append(slashes, '\\');
                line += arg[at];
            }
        }
        line += '"';
    }
    return line;
}

std::string describeProcess(const ProcessOptions& options) {
    std::string text = quoteForLog(utf8(options.executable));
    for (const std::string& argument : options.arguments) text += " " + quoteForLog(argument);
    return text;
}

ProcessResult runProcess(const ProcessOptions& options) {
    ProcessResult result;
    fs::path program;
    if (!resolveExecutable(options, program, result.error)) return result;
    return runResolved(options, program, nullptr);
}

ProcessResult runShellCommand(const std::string& command, const ProcessOptions& base) {
    ProcessOptions options = base;
#if defined(_WIN32)
    wchar_t comspec[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
    options.executable = n && n < MAX_PATH ? fs::path(std::wstring(comspec, n)) : fs::path("cmd.exe");
    options.arguments.clear();
    ProcessResult result;
    fs::path program;
    if (!resolveExecutable(options, program, result.error)) return result;
    // cmd's own rules: /s strips the outer quotes and runs the rest as typed.
    const std::string line = "cmd.exe /d /s /c \"" + command + "\"";
    return runResolved(options, program, &line);
#else
    options.executable = "/bin/sh";
    options.arguments = {"-c", command};
    return runProcess(options);
#endif
}

bool startDetachedProcess(const ProcessOptions& options, std::string& error) {
    fs::path program;
    if (!resolveExecutable(options, program, error)) return false;
#if defined(_WIN32)
    std::vector<std::string> argv{utf8(options.executable)};
    for (const std::string& a : options.arguments) argv.push_back(a);
    Started child;
    if (!startWindows(options, program.wstring(), widen(windowsCommandLine(argv)), true, child, error)) return false;
    CloseHandle(child.info.hThread);
    CloseHandle(child.info.hProcess);
    return true;
#else
    // Forks twice: the grandchild runs the program in a session of its own, adopted by
    // init, so nothing here waits for it or ends it.
    Prepared p;
    prepare(options, program, p);
    int report[2];
    if (!makePipe(report)) {
        error = std::string("pipe failed: ") + std::strerror(errno);
        return false;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        error = std::string("fork failed: ") + std::strerror(errno);
        close(report[0]);
        close(report[1]);
        return false;
    }
    if (pid == 0) {
        const pid_t grandchild = fork();
        if (grandchild == 0) execChild(p, -1, report[1], true);
        _exit(grandchild < 0 ? 1 : 0);
    }
    close(report[1]);
    int status = 0;
    waitpid(pid, &status, 0);
    return readReport(report[0], p, error);
#endif
}

std::filesystem::path currentExecutable() {
    std::error_code ec;
#if defined(_WIN32)
    std::vector<wchar_t> buffer(32768);
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    return fs::path(std::wstring(buffer.data(), size));
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0) return fs::canonical(buffer, ec);
    return {};
#else
    return fs::read_symlink("/proc/self/exe", ec);
#endif
}

}   // namespace shockwave
