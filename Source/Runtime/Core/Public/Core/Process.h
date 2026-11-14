#pragma once

// Child processes without a shell (S12, docs/Diagnostics.md "Child processes").
//
//   shockwave::ProcessOptions run;
//   run.executable = "cmake";                       // a path, or a name found on PATH
//   run.arguments = {"--build", folder.string(), "--target", "player"};
//   run.environment.set("SDL_VIDEO_DRIVER", "offscreen");
//   run.timeoutSeconds = 600;
//   run.onLine = [](std::string_view line) { ... };   // stdout and stderr, line by line
//   const shockwave::ProcessResult result = shockwave::runProcess(run);
//
// Each argument reaches the child exactly as given: no shell sees it, so spaces, quotes,
// '$', ';', '&', '%' and any Unicode are only text. On POSIX the child is exec'd with the
// argument vector; on Windows the command line is encoded the way the C runtime decodes
// it (CommandLineToArgvW's rules), so the child's argv matches.
//
// A timeout or a cancel ends the child and everything it started: on POSIX its own
// process group gets SIGTERM, then SIGKILL two seconds later; on Windows it runs in a job
// object that is terminated. A child that exits on its own leaves anything it started in
// the background alone.
//
// A shell, when a developer command really needs one, is runShellCommand(): a separate,
// explicit call that packaging, verification and game start never use.

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shockwave {

// Changes to the environment the child inherits.
struct ProcessEnvironment {
    bool inherit = true;   // start from this process's environment; false: empty
    std::vector<std::pair<std::string, std::string>> set;   // added or replaced
    std::vector<std::string> unset;   // removed
    void put(std::string name, std::string value) { set.emplace_back(std::move(name), std::move(value)); }
    void remove(std::string name) { unset.push_back(std::move(name)); }
};

struct ProcessOptions {
    std::filesystem::path executable;   // a path, or a bare name looked up on PATH
    std::vector<std::string> arguments;   // UTF-8; argv[0] is added (the executable as given)
    std::filesystem::path workingDirectory;   // empty: this process's
    ProcessEnvironment environment;
    double timeoutSeconds = 0;   // 0: none
    // Each line of stdout and stderr together, without its line ending, as it arrives.
    // Called on the calling thread. Unset: the output is read and dropped.
    std::function<void(std::string_view)> onLine;
    // Polled about every 50 ms: true ends the child (and what it started).
    const std::atomic<bool>* cancel = nullptr;
};

struct ProcessResult {
    bool started = false;   // false: error says why (not found, not executable, bad folder)
    int exitCode = -1;   // the child's own; 128 + n after signal n on POSIX
    int signal = 0;   // POSIX: the signal that ended it, 0 when it exited
    bool timedOut = false;
    bool cancelled = false;
    std::string error;
    bool ok() const { return started && !timedOut && !cancelled && exitCode == 0; }
};

ProcessResult runProcess(const ProcessOptions& options);

// Starts a program that outlives this one (the editor's Run game, a restart): no output is
// read and nothing waits for it. False, with why, when it couldn't start.
bool startDetachedProcess(const ProcessOptions& options, std::string& error);

// A developer command through the system shell (sh -c, or cmd /c). Never for anything
// built from project-controlled names.
ProcessResult runShellCommand(const std::string& command, const ProcessOptions& options = {});

// The executable a bare name resolves to on PATH (with PATHEXT on Windows), or empty.
std::filesystem::path findExecutable(const std::string& name);

// The command line Windows gets for these arguments, for tests and logs: each argument
// quoted only when it needs to be, backslashes doubled only before a quote.
std::string windowsCommandLine(const std::vector<std::string>& argv);

// For logs: the program and its arguments, each quoted when it holds a space or quote.
std::string describeProcess(const ProcessOptions& options);

// This program's own file (/proc/self/exe, GetModuleFileNameW, _NSGetExecutablePath).
std::filesystem::path currentExecutable();

}   // namespace shockwave
