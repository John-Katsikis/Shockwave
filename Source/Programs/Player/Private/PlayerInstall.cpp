// player --verify-install (S13, docs/Release.md#platforms): installs a package the hard way
// and plays it there, then keeps what it saw as evidence for the platform gates.
//
// The package is copied to a read-only folder whose path has spaces and non-ASCII letters,
// and started only through its own entry point (the launcher its marker names), from
// another working folder, with a minimal environment: no LD_LIBRARY_PATH, no developer
// PATH, a fresh home. It passes its own --check, plays each scenario in order (so one can
// save and the next continue), and the player's data must land in that fresh home while
// the install folder stays byte for byte as it was. Nothing in the package or in the
// game's reports is ever run as a command: reports are only read for their values.
#include "Player.h"

#include <Core/Process.h>
#include <Network/Sha256.h>
#include <Packaging/Package.h>
#include <Platform/SystemInfo.h>
#include <Projects/InstallRecord.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>

#if defined(__linux__)
#include <gnu/libc-version.h>
#endif
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace player {

namespace {

constexpr const char* kWorkMarker = "shockwave-verify-install.txt";

using Clock = std::chrono::steady_clock;

double since(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}
fs::path pathOf(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

std::string nowUtc() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

std::string jsonQuote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "\\u%04x", c);
                out += hex;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

// The first "key": "value" in a report the game wrote: read for its value, never run.
std::string reportString(const std::string& text, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\": \"";
    const size_t at = text.find(needle);
    if (at == std::string::npos) return {};
    std::string out;
    for (size_t i = at + needle.size(); i < text.size() && out.size() < 4096; ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            const char next = text[++i];
            out += next == 'n' ? ' ' : next;
        } else if (text[i] == '"') {
            return out;
        } else {
            out += text[i];
        }
    }
    return {};
}
// Its top-level "ok": the first one, before any step's.
bool reportOk(const std::string& text) {
    const size_t at = text.find("\"ok\": ");
    return at != std::string::npos && text.compare(at + 6, 4, "true") == 0;
}

// Every file under `root`, in path order: its path and bytes, so two folders hash the same
// exactly when they hold the same files. Links count as their target text.
struct Digest {
    std::string sha256;
    uint64_t files = 0, bytes = 0;
    std::string error;
};
Digest folderDigest(const fs::path& root) {
    Digest out;
    std::vector<fs::path> entries;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (!it->is_directory(ec) || it->is_symlink(ec)) entries.push_back(it->path());
    if (ec) {
        out.error = "can't read " + utf8(root) + ": " + ec.message();
        return out;
    }
    std::vector<std::pair<std::string, fs::path>> sorted;
    for (const fs::path& p : entries) sorted.emplace_back(utf8(p.lexically_relative(root).generic_u8string()), p);
    std::sort(sorted.begin(), sorted.end());
    shockwave::net::Sha256 hash;
    std::vector<char> buffer(1 << 16);
    for (const auto& [name, path] : sorted) {
        hash.update(name);
        hash.updateU16(0);
        if (fs::is_symlink(path, ec)) {
            hash.update("link:" + utf8(fs::read_symlink(path, ec)));
            ++out.files;
            continue;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            out.error = "can't read " + name;
            return out;
        }
        uint64_t size = 0;
        while (in) {
            in.read(buffer.data(), std::streamsize(buffer.size()));
            const std::streamsize got = in.gcount();
            if (got <= 0) break;
            hash.update(std::span(reinterpret_cast<const uint8_t*>(buffer.data()), size_t(got)));
            size += uint64_t(got);
        }
        hash.updateU64(size);
        ++out.files;
        out.bytes += size;
    }
    out.sha256 = shockwave::net::toHex(hash.finish());
    return out;
}

std::string fileSha256(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    shockwave::net::Sha256 hash;
    std::vector<char> buffer(1 << 16);
    while (in) {
        in.read(buffer.data(), std::streamsize(buffer.size()));
        if (in.gcount() <= 0) break;
        hash.update(std::span(reinterpret_cast<const uint8_t*>(buffer.data()), size_t(in.gcount())));
    }
    return shockwave::net::toHex(hash.finish());
}

// Write permission off (or back on) for everything under root.
void setWritable(const fs::path& root, bool writable) {
    std::error_code ec;
    const fs::perms write = fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write;
    std::vector<fs::path> all{root};
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (!it->is_symlink(ec)) all.push_back(it->path());
    // Folders last when locking (their files first), first when unlocking.
    if (!writable) std::reverse(all.begin(), all.end());
    for (const fs::path& p : all)
        fs::permissions(
            p, writable ? fs::perms::owner_write : write, writable ? fs::perm_options::add : fs::perm_options::remove,
            ec);
}

// A minimal environment, as a fresh account would have: a few display, audio and driver
// variables pass through; the developer's PATH, library paths and engine settings don't.
shockwave::ProcessEnvironment cleanEnvironment(const fs::path& home, const fs::path& temp) {
    shockwave::ProcessEnvironment env;
    env.inherit = false;
    static const char* const passed[] = {
        "DISPLAY",
        "WAYLAND_DISPLAY",
        "XDG_RUNTIME_DIR",
        "XDG_SESSION_TYPE",
        "DBUS_SESSION_BUS_ADDRESS",
        "PULSE_SERVER",
        "PIPEWIRE_RUNTIME_DIR",
        "SDL_VIDEO_DRIVER",
        "SDL_AUDIO_DRIVER",
        "LANG",
        "LC_ALL",
        "TZ",
        // Which Vulkan driver to load (a test machine's choice, not the engine's).
        "VK_ICD_FILENAMES",
        "VK_DRIVER_FILES",
        "VK_ADD_DRIVER_FILES",
        "__NV_PRIME_RENDER_OFFLOAD",
        "__GLX_VENDOR_LIBRARY_NAME",
        "DRI_PRIME",
#if defined(_WIN32)
        "SystemRoot",
        "windir",
        "SystemDrive",
        "PATHEXT",
        "COMSPEC",
        "NUMBER_OF_PROCESSORS",
        "PROCESSOR_ARCHITECTURE",
#endif
    };
    for (const char* name : passed)
        if (const char* value = std::getenv(name)) env.put(name, value);
#if defined(_WIN32)
    const std::string root = utf8(home);
    env.put("USERPROFILE", root);
    env.put("APPDATA", utf8(home / "AppData" / "Roaming"));
    env.put("LOCALAPPDATA", utf8(home / "AppData" / "Local"));
    env.put("TEMP", utf8(temp));
    env.put("TMP", utf8(temp));
    if (const char* system = std::getenv("SystemRoot")) env.put("PATH", std::string(system) + "\\System32;" + system);
#else
    env.put("HOME", utf8(home));
    env.put("TMPDIR", utf8(temp));
    env.put("PATH", "/usr/bin:/bin");
#endif
    return env;
}

std::string distribution() {
#if defined(__linux__)
    std::ifstream in("/etc/os-release");
    for (std::string line; std::getline(in, line);)
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            std::string value = line.substr(12);
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);
            return value;
        }
#endif
    return {};
}

std::string glibcVersion() {
#if defined(__linux__)
    return gnu_get_libc_version();
#else
    return {};
#endif
}

bool runningAsRoot() {
#if defined(_WIN32)
    return false;
#else
    return geteuid() == 0;
#endif
}

struct Verifier {
    const InstallCommand& command;
    shockwave::InstallRecord record;
    fs::path work, installRoot, installed, home, temp, cwd, reports, scenarios;
    shockwave::PackageInfo info;
    std::string installDigest;

    void step(std::string name, shockwave::InstallStepResult result, double seconds, std::string detail) {
        std::printf(
            "%-8s %s (%.1f s): %s\n", shockwave::installStepResultName(result), name.c_str(), seconds, detail.c_str());
        record.steps.push_back({std::move(name), result, seconds, std::move(detail)});
    }
    bool failed() const {
        return std::any_of(record.steps.begin(), record.steps.end(), [](const shockwave::InstallStep& s) {
            return s.result == shockwave::InstallStepResult::Failed;
        });
    }

    // Runs the installed game's entry point, the way a player would start it.
    shockwave::ProcessResult launch(std::vector<std::string> arguments, double timeout, std::deque<std::string>& tail) {
        shockwave::ProcessOptions options;
        options.executable = shockwave::packageEntryPoint(installed, info);
        options.arguments = std::move(arguments);
        options.workingDirectory = cwd;
        options.environment = cleanEnvironment(home, temp);
        options.timeoutSeconds = timeout;
        options.onLine = [&](std::string_view line) {
            if (command.verbose) std::printf("  | %.*s\n", int(line.size()), line.data());
            tail.emplace_back(line);
            if (tail.size() > 30) tail.pop_front();
        };
        return shockwave::runProcess(options);
    }
    static std::string why(const shockwave::ProcessResult& r, const std::deque<std::string>& tail) {
        std::string out = !r.started   ? "didn't start: " + r.error
                          : r.timedOut ? std::string("timed out")
                          : r.signal   ? "ended by signal " + std::to_string(r.signal)
                                       : "exit code " + std::to_string(r.exitCode);
        if (!tail.empty()) out += "; last line: " + tail.back();
        return out;
    }
    static void printTail(const std::deque<std::string>& tail) {
        for (const std::string& line : tail) std::fprintf(stderr, "  | %s\n", line.c_str());
    }
};

}   // namespace

int verifyInstall(const InstallCommand& command, const std::string& renderer, const fs::path& json) {
    const auto start = Clock::now();
    Verifier v{command};
    std::string error;
    std::error_code ec;

    // ---- What's being verified ----
    const fs::path package = fs::absolute(command.package, ec).lexically_normal();
    if (!shockwave::readPackageInfo(package, v.info, error)) {
        std::fprintf(stderr, "error: %s: %s\n", utf8(package).c_str(), error.c_str());
        return 1;
    }
    if (v.info.platform != shockwave::packagePlatform()) {
        std::fprintf(
            stderr, "error: %s is a %s package; this machine runs %s packages\n", utf8(package).c_str(),
            v.info.platform.c_str(), shockwave::packagePlatform());
        return 1;
    }
    if (shockwave::packageEntryPoint(package, v.info).empty()) {
        std::fprintf(
            stderr, "error: %s doesn't name the program players start (an older packager made it); package it again\n",
            utf8(package).c_str());
        return 1;
    }
    std::vector<fs::path> scenarioFiles = command.scenarios;
    if (scenarioFiles.empty()) {
        std::fprintf(
            stderr, "error: --verify-install needs a scenario to play: --flow-test file.json (repeat it to "
                    "play several in order), or --project with a release contract naming one\n");
        return 2;
    }
    std::vector<shockwave::FlowScenario> parsed;
    for (const fs::path& file : scenarioFiles) {
        std::ifstream in(file, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        shockwave::FlowScenario scenario;
        if (!in || !shockwave::parseFlowScenario(text.str(), scenario, error)) {
            std::fprintf(stderr, "error: %s: %s\n", utf8(file).c_str(), in ? error.c_str() : "can't read it");
            return 1;
        }
        parsed.push_back(std::move(scenario));
    }

    // ---- Where: a work folder only this tool makes and removes ----
    v.work = command.workFolder.empty() ? fs::temp_directory_path(ec) / ("shockwave-verify-install-" + std::to_string(
#if defined(_WIN32)
                                                                                                           _getpid()
#else
                                                                                                           getpid()
#endif
                                                                                                               ))
                                        : fs::absolute(command.workFolder, ec);
    if (fs::exists(v.work, ec)) {
        if (!fs::exists(v.work / kWorkMarker, ec) && !fs::is_empty(v.work, ec)) {
            std::fprintf(
                stderr, "error: %s isn't empty and wasn't made by --verify-install; pick another --install-dir\n",
                utf8(v.work).c_str());
            return 1;
        }
        setWritable(v.work, true);
        fs::remove_all(v.work, ec);
    }
    fs::create_directories(v.work, ec);
    std::ofstream(v.work / kWorkMarker) << "Made by player --verify-install; it removes this folder.\n";
    v.installRoot = v.work / pathOf("Installed Gämes 名前");
    v.installed = v.installRoot / package.filename();
    v.home = v.work / pathOf("home ü");
    v.temp = v.work / "tmp";
    v.cwd = v.work / pathOf("another folder");
    v.reports = v.work / "reports";
    v.scenarios = v.work / "scenarios";
    for (const fs::path& f : {v.installRoot, v.home, v.temp, v.cwd, v.reports, v.scenarios})
        fs::create_directories(f, ec);
    if (ec) {
        std::fprintf(stderr, "error: can't make %s: %s\n", utf8(v.work).c_str(), ec.message().c_str());
        return 1;
    }

    shockwave::InstallRecord& r = v.record;
    const shockwave::SystemInfo& system = shockwave::systemInfo();
    r.machine = command.machine;
    r.recorded = nowUtc();
    r.platform = v.info.platform;
    r.os = system.os + (system.osVersion.empty() ? "" : " " + system.osVersion);
    r.distribution = distribution();
    r.glibc = glibcVersion();
    r.cpu = system.cpu;
    r.package = utf8(package.filename());
    r.version = v.info.version;
    r.buildConfig = v.info.buildType;
    r.buildId = v.info.buildId;
    r.contentId = v.info.contentId;
    std::printf(
        "verify-install: %s %s (%s, %s%s)\n", v.info.name.c_str(), v.info.version.c_str(), r.platform.c_str(),
        r.buildConfig.c_str(), r.buildId.empty() ? ", development package" : (", build " + r.buildId).c_str());
    std::printf(
        "machine:        %s; %s%s\n", r.machine.empty() ? "(unnamed)" : r.machine.c_str(), r.os.c_str(),
        r.distribution.empty() ? "" : ("; " + r.distribution).c_str());

    // ---- install: an exact copy, read-only, at an awkward path ----
    {
        const auto t = Clock::now();
        const Digest source = folderDigest(package);
        if (!source.error.empty()) {
            v.step("install", shockwave::InstallStepResult::Failed, since(t), source.error);
        } else {
            r.packageSha256 = source.sha256;
            r.files = source.files;
            r.bytes = source.bytes;
            if (const fs::path archive = pathOf(utf8(package) + ".tar.gz"); fs::is_regular_file(archive, ec))
                r.archiveSha256 = fileSha256(archive);
            fs::copy(package, v.installed, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
            const Digest copy = ec ? Digest{} : folderDigest(v.installed);
            if (ec || copy.sha256 != source.sha256) {
                v.step(
                    "install", shockwave::InstallStepResult::Failed, since(t),
                    ec ? "couldn't copy the package: " + ec.message() : "the copy doesn't match the package");
            } else {
                setWritable(v.installed, false);
                v.installDigest = copy.sha256;
                r.readOnly = true;
                r.spacesInPath = r.unicodePath = r.otherFolder = r.cleanEnvironment = true;
                std::ostringstream detail;
                detail << "copied " << source.files << " files (" << (source.bytes >> 20)
                       << " MiB) to a read-only folder with spaces and non-ASCII letters in its path; started from "
                          "another folder with a minimal environment";
                if (runningAsRoot())
                    detail << " (as root, so the system doesn't enforce read-only: the folder is checked "
                              "unchanged afterwards instead)";
                v.step("install", shockwave::InstallStepResult::Passed, since(t), detail.str());
            }
        }
    }

    // ---- check: the installed game finds its project, code and plugins ----
    if (!v.failed()) {
        const auto t = Clock::now();
        std::deque<std::string> tail;
        const shockwave::ProcessResult result = v.launch({"--check"}, 300, tail);
        if (result.ok())
            v.step("check", shockwave::InstallStepResult::Passed, since(t), "its own --check passed");
        else {
            Verifier::printTail(tail);
            v.step("check", shockwave::InstallStepResult::Failed, since(t), "--check: " + Verifier::why(result, tail));
        }
    }

    // ---- scenarios, in order, sharing the player's data ----
    for (size_t i = 0; i < scenarioFiles.size() && !v.failed(); ++i) {
        const auto t = Clock::now();
        const std::string name = "scenario " + utf8(scenarioFiles[i].filename());
        // The scenario's own copy, so nothing is read from the engine checkout.
        const fs::path copy = v.scenarios / (std::to_string(i + 1) + "-" + utf8(scenarioFiles[i].filename()));
        fs::copy_file(scenarioFiles[i], copy, fs::copy_options::overwrite_existing, ec);
        const fs::path report = v.reports / (std::to_string(i + 1) + ".json");
        std::vector<std::string> args = {"--flow-test", utf8(copy), "--json", utf8(report)};
        if (!renderer.empty()) {
            args.push_back("--renderer");
            args.push_back(renderer);
        }
        std::deque<std::string> tail;
        const shockwave::ProcessResult result = v.launch(args, parsed[i].timeout + 120, tail);
        std::ifstream in(report, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        const std::string json = text.str();
        if (!result.ok() || json.empty() || !reportOk(json)) {
            Verifier::printTail(tail);
            const std::string message = reportString(json, "message");
            v.step(
                name, shockwave::InstallStepResult::Failed, since(t),
                json.empty() ? "no report: " + Verifier::why(result, tail)
                             : "the flow test failed" + (message.empty() ? std::string() : ": " + message));
            continue;
        }
        // The game that ran is this package, as built.
        const std::string ranBuild = reportString(json, "build_id");
        if (reportString(json, "package_project").empty() || ranBuild != v.info.buildId) {
            v.step(
                name, shockwave::InstallStepResult::Failed, since(t),
                "the report doesn't come from this package (build \"" + ranBuild + "\", expected \"" + v.info.buildId +
                    "\")");
            continue;
        }
        r.renderer = reportString(json, "renderer");
        r.gpu = reportString(json, "gpu");
        r.gpuKind = reportString(json, "gpu_kind");
        r.gpuDriver = reportString(json, "gpu_driver");
        v.step(
            name, shockwave::InstallStepResult::Passed, since(t),
            std::to_string(parsed[i].steps.size()) + " steps on the " + (r.renderer.empty() ? "?" : r.renderer) +
                " renderer" + (r.gpu.empty() ? std::string() : " (" + r.gpu + ")"));
    }

    // ---- user data: in the player's own folders; the install untouched ----
    if (!v.failed()) {
        const auto t = Clock::now();
        uint64_t written = 0;
        for (auto it = fs::recursive_directory_iterator(v.home, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
            if (it->is_regular_file(ec)) ++written;
        const Digest after = folderDigest(v.installed);
        if (after.sha256 != v.installDigest)
            v.step(
                "user data", shockwave::InstallStepResult::Failed, since(t),
                "the game changed its own install folder; players' data belongs in their per-user folders");
        else if (written == 0)
            v.step(
                "user data", shockwave::InstallStepResult::Failed, since(t),
                "nothing reached the player's per-user folders (no save, settings or log)");
        else
            v.step(
                "user data", shockwave::InstallStepResult::Passed, since(t),
                std::to_string(written) + " files in the player's per-user folders; the install folder is unchanged");
    }
    // S13 part 2 brings these; skipped says so in the record rather than leaving it to guess.
    v.step("update", shockwave::InstallStepResult::Skipped, 0, "not verified yet (S13 part 2)");
    v.step("reinstall", shockwave::InstallStepResult::Skipped, 0, "not verified yet (S13 part 2)");
    r.passed = !v.failed();

    // ---- What it showed ----
    if (!command.machine.empty()) {
        const fs::path file = command.recordFolder / shockwave::installFileName(command.machine, r.platform);
        if (shockwave::writeInstall(file, r, error))
            std::printf("Wrote %s\n", utf8(file).c_str());
        else
            std::fprintf(stderr, "error: %s\n", error.c_str());
    }
    if (!json.empty()) {
        std::string out = "{\n  \"format\": 1,\n  \"mode\": \"verify_install\",\n";
        out += std::string("  \"ok\": ") + (r.passed ? "true" : "false") + ",\n";
        out += "  \"package\": " + jsonQuote(r.package) + ",\n  \"version\": " + jsonQuote(r.version) +
               ",\n  \"platform\": " + jsonQuote(r.platform) + ",\n  \"build_config\": " + jsonQuote(r.buildConfig) +
               ",\n  \"build_id\": " + jsonQuote(r.buildId) + ",\n  \"package_sha256\": " + jsonQuote(r.packageSha256) +
               ",\n  \"archive_sha256\": " + jsonQuote(r.archiveSha256) + ",\n  \"files\": " + std::to_string(r.files) +
               ",\n  \"bytes\": " + std::to_string(r.bytes) + ",\n  \"machine\": " + jsonQuote(r.machine) +
               ",\n  \"os\": " + jsonQuote(r.os) + ",\n  \"distribution\": " + jsonQuote(r.distribution) +
               ",\n  \"renderer\": " + jsonQuote(r.renderer) + ",\n  \"gpu\": " + jsonQuote(r.gpu) +
               ",\n  \"gpu_kind\": " + jsonQuote(r.gpuKind) + ",\n  \"steps\": [";
        for (size_t i = 0; i < r.steps.size(); ++i) {
            const shockwave::InstallStep& s = r.steps[i];
            char seconds[32];
            std::snprintf(seconds, sizeof(seconds), "%.2f", s.seconds);
            out += std::string(i ? ",\n    " : "\n    ") + "{\"name\": " + jsonQuote(s.name) +
                   ", \"result\": " + jsonQuote(shockwave::installStepResultName(s.result)) +
                   ", \"seconds\": " + seconds + ", \"detail\": " + jsonQuote(s.detail) + "}";
        }
        out += "\n  ]\n}\n";
        std::ofstream file(json, std::ios::binary);
        file << out;
        file.close();
        if (!file) std::fprintf(stderr, "error: can't write %s\n", utf8(json).c_str());
    }

    // ---- Leave nothing behind (unless asked to) ----
    setWritable(v.work, true);
    if (command.keep)
        std::printf("Kept %s\n", utf8(v.work).c_str());
    else
        fs::remove_all(v.work, ec);
    std::printf("verify-install %s in %.1f s\n", r.passed ? "passed" : "failed", since(start));
    return r.passed ? 0 : 1;
}

}   // namespace player
