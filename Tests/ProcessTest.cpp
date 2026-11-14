// Child processes without a shell (Core/Process.h, docs/Diagnostics.md): arguments that
// look hostile reach the child byte for byte and are never run; the environment, working
// directory, exit code and output lines; a missing program; a timeout and a cancel that
// end the child and what it started; a detached start; Windows command-line encoding.
//
//   process_test <process_child> <temp folder>
#include <Core/Process.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using shockwave::ProcessOptions;
using shockwave::ProcessResult;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace {

fs::path child;
fs::path temp;

std::string hex(const std::string& text) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const unsigned char c : text) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out.empty() ? "-" : out;
}

fs::path pathOf(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

ProcessResult run(std::vector<std::string> arguments, std::vector<std::string>& lines, ProcessOptions options = {}) {
    options.executable = child;
    options.arguments = std::move(arguments);
    lines.clear();
    options.onLine = [&](std::string_view line) { lines.emplace_back(line); };
    return shockwave::runProcess(options);
}

bool alive(long pid) {
#if defined(_WIN32)
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!process) return false;
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
#else
    return kill(pid_t(pid), 0) == 0;
#endif
}

void hostileArguments() {
    // Shell syntax for every shell we know of, as plain text: none of it may run.
    const fs::path marker = temp / "PWNED";
    const std::string m = marker.string();
    const std::vector<std::string> hostile = {
        "plain",
        "two words",
        "",
        " leading and trailing ",
        "$(touch " + m + ")",
        "`touch " + m + "`",
        "; touch " + m,
        "&& echo hi > " + m,
        "a & b | c > d < e",
        "\"quoted\"",
        "'single'",
        "back\\slash",
        "trailing\\",
        "trailing\\\\",
        "\\\"",
        "quote\" inside",
        "100%PATH%",
        "%%",
        "^caret^",
        "!bang!",
        "tab\there",
        "new\nline",
        "Ünïcødé 名前 🎮",
        "*",
        "~",
    };
    std::vector<std::string> lines;
    std::vector<std::string> arguments = {"args"};
    arguments.insert(arguments.end(), hostile.begin(), hostile.end());
    const ProcessResult result = run(arguments, lines);
    CHECK(result.ok());
    CHECK(lines.size() == hostile.size());
    for (size_t i = 0; i < hostile.size() && i < lines.size(); ++i) {
        if (lines[i] != hex(hostile[i]))
            std::fprintf(
                stderr, "argument %zu came through as %s, not %s\n", i, lines[i].c_str(), hex(hostile[i]).c_str());
        CHECK(lines[i] == hex(hostile[i]));
    }
    std::error_code ec;
    CHECK(!fs::exists(marker, ec));
}

void environmentAndFolder() {
    std::vector<std::string> lines;
    ProcessOptions options;
    options.environment.put("SHOCKWAVE_PROCESS_TEST", "a value; with $(things)");
    options.environment.put("SHOCKWAVE_PROCESS_UNICODE", "ναι");
    options.environment.remove("SHOCKWAVE_PROCESS_GONE");
#if defined(_WIN32)
    SetEnvironmentVariableW(L"SHOCKWAVE_PROCESS_GONE", L"here");
#else
    setenv("SHOCKWAVE_PROCESS_GONE", "here", 1);
#endif
    CHECK(run({"env", "SHOCKWAVE_PROCESS_TEST", "SHOCKWAVE_PROCESS_UNICODE", "SHOCKWAVE_PROCESS_GONE", "PATH"}, lines,
              options)
              .ok());
    CHECK(lines.size() == 4);
    if (lines.size() == 4) {
        CHECK(lines[0] == "SHOCKWAVE_PROCESS_TEST=a value; with $(things)");
        CHECK(lines[1] == "SHOCKWAVE_PROCESS_UNICODE=ναι");
        CHECK(lines[2] == "SHOCKWAVE_PROCESS_GONE unset");
        CHECK(lines[3].rfind("PATH=", 0) == 0);   // inherited
    }

    // Not inherited: only what's set.
    options = {};
    options.environment.inherit = false;
    options.environment.put("ONLY", "this");
    CHECK(run({"env", "ONLY", "SHOCKWAVE_PROCESS_GONE"}, lines, options).ok());
    CHECK(lines.size() == 2 && lines[0] == "ONLY=this" && lines[1] == "SHOCKWAVE_PROCESS_GONE unset");

    // A working directory whose name a shell would trip over.
    const fs::path folder = temp / pathOf("a folder; with $x & 'quotes' 100% ü");
    std::error_code ec;
    fs::create_directories(folder, ec);
    options = {};
    options.workingDirectory = folder;
    CHECK(run({"cwd"}, lines, options).ok());
    const std::u8string expected = fs::canonical(folder, ec).u8string();
    CHECK(lines.size() == 1);
    if (lines.size() == 1) {
        const std::u8string got = fs::canonical(fs::path(lines[0] == "-" ? std::u8string() : [&] {
            std::u8string text;
            for (size_t i = 0; i + 1 < lines[0].size(); i += 2)
                text += char8_t(std::stoi(lines[0].substr(i, 2), nullptr, 16));
            return text;
        }()), ec).u8string();
        CHECK(got == expected);
    }

    // A folder that doesn't exist: it doesn't start, and says why.
    options.workingDirectory = temp / "missing folder";
    const ProcessResult missing = run({"cwd"}, lines, options);
    CHECK(!missing.started || missing.exitCode != 0);
}

void outputAndExit() {
    std::vector<std::string> lines;
    CHECK(run({"lines"}, lines).ok());
    CHECK(lines.size() == 4);
    if (lines.size() == 4) {
        CHECK(lines[0] == "one");
        CHECK(lines[1] == "two");   // no \r left
        CHECK(lines[2] == "three on stderr");
        CHECK(lines[3] == "four without an end");
    }
    const ProcessResult seven = run({"exit", "7"}, lines);
    CHECK(seven.started && seven.exitCode == 7 && !seven.ok());

    ProcessOptions options;
    options.executable = "shockwave-no-such-program-anywhere";
    const ProcessResult absent = shockwave::runProcess(options);
    CHECK(!absent.started);
    CHECK(absent.error.find("wasn't found on PATH") != std::string::npos);
    options.executable = temp / "no such file";
    CHECK(!shockwave::runProcess(options).started);

    // The developer shell: separate, and explicit.
    lines.clear();
    ProcessOptions shell;
    shell.onLine = [&](std::string_view line) { lines.emplace_back(line); };
    CHECK(shockwave::runShellCommand("echo from the shell", shell).ok());
    CHECK(!lines.empty() && lines[0].find("from the shell") != std::string::npos);
}

void timeoutAndCancel() {
    // A timeout ends the child and the grandchild it started.
    const fs::path pidFile = temp / "grandchild.pid";
    std::error_code ec;
    fs::remove(pidFile, ec);
    std::vector<std::string> lines;
    ProcessOptions options;
    options.timeoutSeconds = 2;
    const auto start = std::chrono::steady_clock::now();
    const ProcessResult result = run({"tree", pidFile.string()}, lines, options);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(result.started && result.timedOut && !result.ok());
    CHECK(seconds < 20);
    long grandchild = 0;
    std::ifstream(pidFile) >> grandchild;
    CHECK(grandchild > 0);
    bool gone = false;
    for (int i = 0; i < 100 && grandchild > 0; ++i) {
        if (!alive(grandchild)) {
            gone = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!gone) std::fprintf(stderr, "the grandchild %ld outlived the timeout\n", grandchild);
    CHECK(gone);

    // A cancel from another thread.
    std::atomic<bool> cancel{false};
    options = {};
    options.cancel = &cancel;
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancel = true;
    });
    const auto cancelStart = std::chrono::steady_clock::now();
    const ProcessResult cancelled = run({"sleep", "30"}, lines, options);
    canceller.join();
    CHECK(cancelled.started && cancelled.cancelled && !cancelled.ok());
    CHECK(std::chrono::steady_clock::now() - cancelStart < std::chrono::seconds(10));
    CHECK(!lines.empty() && lines[0] == "sleeping");
}

void detached() {
    const fs::path file = temp / pathOf("detached ü.txt");
    std::error_code ec;
    fs::remove(file, ec);
    ProcessOptions options;
    options.executable = child;
    options.arguments = {"write", std::string(reinterpret_cast<const char*>(file.u8string().c_str()))};
    std::string error;
    CHECK(shockwave::startDetachedProcess(options, error));
    bool written = false;
    for (int i = 0; i < 200 && !written; ++i) {
        written = fs::exists(file, ec) && fs::file_size(file, ec) > 0;
        if (!written) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(written);
    options.executable = temp / "no such program";
    CHECK(!shockwave::startDetachedProcess(options, error));
    CHECK(!error.empty());
}

void commandLines() {
    using shockwave::windowsCommandLine;
    CHECK(windowsCommandLine({"a", "b"}) == "a b");
    CHECK(windowsCommandLine({"two words", "c"}) == "\"two words\" c");
    CHECK(windowsCommandLine({""}) == "\"\"");
    CHECK(windowsCommandLine({"a\"b"}) == "\"a\\\"b\"");
    CHECK(windowsCommandLine({"a\\b"}) == "a\\b");
    CHECK(windowsCommandLine({"x y\\"}) == "\"x y\\\\\"");
    CHECK(windowsCommandLine({"\\\""}) == "\"\\\\\\\"\"");
    CHECK(windowsCommandLine({"100%PATH%", "a&b"}) == "100%PATH% a&b");
    ProcessOptions options;
    options.executable = "tool";
    options.arguments = {"two words", "plain"};
    CHECK(shockwave::describeProcess(options) == "tool \"two words\" plain");
    CHECK(!shockwave::currentExecutable().empty());
}

}   // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: process_test <process_child> <temp folder>\n");
        return 2;
    }
    child = argv[1];
    temp = argv[2];
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp, ec);
    commandLines();
    hostileArguments();
    environmentAndFolder();
    outputAndExit();
    timeoutAndCancel();
    detached();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("process: all checks passed\n");
    return 0;
}
