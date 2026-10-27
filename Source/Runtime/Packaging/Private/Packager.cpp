// packageProject: build, check, copy, verify and archive a game folder for players.
#include "BinaryInfo.h"
#include "ChildProcess.h"

#include <Core/Plugin.h>
#include <Packaging/Package.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#if defined(__linux__)
#include <link.h>
#elif defined(__APPLE__)
#include <Availability.h>
#endif

namespace shockwave {

namespace fs = std::filesystem;
using packaging::shellQuote;

namespace {

constexpr const char* kMarker = "shockwave-package.txt";   // in Binaries/, names the project

// Where everything goes inside a package, so other platforms can change it in one place.
struct Layout {
    const char* binaries = "Binaries";
    const char* game = "Game";
    const char* cppRuntime = "Binaries/cpp-runtime";
#if defined(_WIN32)
    const char* executableSuffix = ".exe";
#else
    const char* executableSuffix = "";
#endif
    // Libraries next to the player that ship with it (engine modules, SDL).
    static bool library(const fs::path& file) {
        const std::string name = file.filename().string();
#if defined(_WIN32)
        return file.extension() == ".dll";
#elif defined(__APPLE__)
        return file.extension() == ".dylib";
#else
        return name.find(".so") != std::string::npos &&
               (file.extension() == ".so" || name.find(".so.") != std::string::npos);
#endif
    }
};
const Layout kLayout;

// Libraries every Linux system has, so a package may need them without shipping them.
bool systemLibrary(const std::string& name) {
    static const std::set<std::string> known = {
        "libc.so.6",      "libm.so.6",     "libdl.so.2",           "libpthread.so.0",       "librt.so.1",
        "libstdc++.so.6", "libgcc_s.so.1", "ld-linux-x86-64.so.2", "ld-linux-aarch64.so.1", "libvulkan.so.1"};
    return known.count(name) > 0;
}

// '*' is any run of characters other than '/', '?' any one character.
bool globMatch(std::string_view pattern, std::string_view text) {
    size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' ? text[t] != '/' : pattern[p] == text[t])) {
            ++p, ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string_view::npos && text[mark] != '/') {
            p = star + 1;
            t = ++mark;
        } else
            return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

class Packager {
public:
    Packager(const Project& project, const PackageSettings& settings, const PackageOptions& options)
        : m_project(project), m_settings(settings), m_options(options) {}

    PackageResult run() {
        const auto start = std::chrono::steady_clock::now();
        try {
            package();
            m_result.ok = true;
        } catch (const Failure& failure) {
            m_result.error = failure.message;
            std::error_code ec;
            if (!m_staging.empty()) fs::remove_all(m_staging, ec);
            log(LogLevel::Error, failure.message);
        }
        m_result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return m_result;
    }

private:
    struct Failure {
        std::string message;
    };
    struct CopyItem {
        fs::path from, to;
        uintmax_t bytes = 0;
        bool link = false;
    };

    [[noreturn]] void fail(std::string message) { throw Failure{std::move(message)}; }
    void log(LogLevel level, std::string_view text) {
        if (level == LogLevel::Warning) m_result.warnings.emplace_back(text);
        if (m_options.log) m_options.log(level, text);
    }
    void progress(PackageStep step, float fraction) {
        if (m_options.progress) m_options.progress(step, fraction);
    }
    void checkCancel() {
        if (m_options.cancel && m_options.cancel->load()) fail("Packaging was cancelled");
    }
    // Runs a command, sending its output to the log; returns the exit code.
    int run(const std::string& command, LogLevel level = LogLevel::Info) {
        return packaging::runCommand(command, [&](std::string_view line) { log(level, line); });
    }

    void package() {
        m_engine = m_options.engineFolder.empty() ? executableDir() : m_options.engineFolder;
        m_player = m_engine / (std::string("player") + kLayout.executableSuffix);
        if (packagedProject()) fail("This is a packaged game. Package from the engine's build instead");
        if (const auto problems = validatePackageSettings(m_project, m_settings); !problems.empty())
            fail(problems.front());
        checkSteam();
        if (std::string(SHOCKWAVE_BUILD_CONFIG) == "Debug")
            log(LogLevel::Warning,
                "This engine is a Debug build, so the packaged game will run slowly. Package from a Release build "
                "(cmake -B build -DCMAKE_BUILD_TYPE=Release)");

        const fs::path output = m_settings.outputFolder(m_project);
        const std::string folderName = packageFolderName(m_project, m_settings);
        m_final = output / folderName;
        m_staging = output / (folderName + ".partial");
        m_executable = m_settings.executableName(m_project) + kLayout.executableSuffix;
        std::error_code ec;
        if (fs::exists(m_final, ec) && !fs::exists(m_final / kLayout.binaries / kMarker, ec))
            fail(m_final.string() + " already exists and isn't a Shockwave package, so it won't be replaced");
        fs::create_directories(output, ec);
        if (ec) fail("Can't create " + output.string() + ": " + ec.message());
        fs::remove_all(m_staging, ec);

        log(LogLevel::Info,
            "Packaging " + m_settings.displayName(m_project) + " " + m_settings.version + " for " + packagePlatform());
        build();
        check();
        compressTextures();
        copy();
        verify();
        finish();
    }

    // ---- Steam --------------------------------------------------------------------------
    // A Steam game needs its own AppID; 480 (Spacewar, Valve's shared test app) only
    // goes into a development package. An engine built without the SDK still packages
    // the game, which then plays offline only.
    void checkSteam() {
        std::error_code ec;
        m_steamEngine = fs::exists(m_engine / steamRuntimeLibrary(), ec);
        const PackageSteam& steam = m_options.steam;
        if (!steam.uses) return;
        if (steam.appId == 0)
            fail("The game plays over Steam but has no steam_app_id; set it in Config/Network.swnetwork");
        if (steam.appId == 480 && !steam.development)
            fail("steam_app_id is 480, Valve's shared test app (Spacewar). Set your game's own AppID in "
                 "Config/Network.swnetwork, or make a development package to test with it (player --package "
                 "--dev-package, or Development package in the editor)");
        if (steam.appId == 480)
            log(LogLevel::Warning,
                "Development package with AppID 480: for testing only; don't upload it to Steam or give it to "
                "players");
        if (!m_steamEngine)
            log(LogLevel::Warning,
                "This engine was built without the Steamworks SDK, so the package plays offline only. Package from "
                "a build configured with -DSHOCKWAVE_STEAMWORKS=ON for online play");
    }

    // ---- Build --------------------------------------------------------------------------
    void build() {
        progress(PackageStep::Build, 0.0f);
        std::error_code ec;
        const fs::path folder = m_options.buildFolder ? *m_options.buildFolder : fs::path(SHOCKWAVE_BUILD_DIR);
        if (!m_settings.build) {
            log(LogLevel::Info, "Not building first (build false): packaging the engine as it is");
        } else if (folder.empty() || !fs::exists(folder / "CMakeCache.txt", ec)) {
            log(LogLevel::Info, "No build folder to build in: packaging the engine as it is");
        } else if (!fs::equivalent(folder / "bin", m_engine, ec)) {
            log(LogLevel::Info, "The engine isn't from " + folder.string() + ": packaging it as it is");
        } else {
            // The player depends on every plugin and game module, so this builds all of
            // what ships.
            log(LogLevel::Info, "Building the player and game code in " + folder.string());
            const int code = run("cmake --build " + shellQuote(folder) + " --target player");
            if (code != 0) fail("The build failed (exit code " + std::to_string(code) + "); the errors are above");
        }
        checkCancel();
        progress(PackageStep::Build, 1.0f);
    }

    // ---- Check --------------------------------------------------------------------------
    // The same checks as `player --check`, so a broken scene never ships.
    void check() {
        progress(PackageStep::Check, 0.0f);
        std::error_code ec;
        if (!fs::exists(m_player, ec))
            fail("There's no player at " + m_player.string() + " to package. Build it first");
        log(LogLevel::Info, "Checking the project");
        std::vector<std::string> lines;
        const int code = packaging::runCommand(
            headless() + shellQuote(m_player) + " --check " + shellQuote(m_project.file),
            [&](std::string_view line) { lines.emplace_back(line); });
        for (const std::string& line : lines) {
            const bool fine = line.rfind("ok ", 0) == 0 || line.rfind("0 problems", 0) == 0;
            log(fine || code == 0 ? LogLevel::Info : LogLevel::Error, line);
        }
        if (code != 0) fail("The project has problems (listed above); fix them, then package again");
        checkCancel();
        progress(PackageStep::Check, 1.0f);
    }

    // Every texture compressed into the project's cache now (docs/Optimisation.md), so the
    // game ships with them and players' machines never compress anything.
    void compressTextures() {
        if (m_project.textureCompression == "off") return;
        log(LogLevel::Info, "Compressing textures");
        const int code = packaging::runCommand(
            headless() + shellQuote(m_player) + " --optimise " + shellQuote(m_project.file),
            [&](std::string_view line) { log(LogLevel::Info, line); });
        // Not fatal: players' machines compress what's missing on first run.
        if (code != 0) log(LogLevel::Warning, "Some textures couldn't be compressed; players' machines will do those");
        checkCancel();
    }

    // Tools run without a window or sound device, so packaging works on a headless
    // machine (CI, an AI agent) too.
    static std::string headless() {
#if defined(_WIN32)
        return "";
#else
        return "env -u LD_LIBRARY_PATH SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy ";
#endif
    }

    // ---- Copy ---------------------------------------------------------------------------
    void copy() {
        progress(PackageStep::Copy, 0.0f);
        std::vector<CopyItem> items;
        const fs::path binaries = m_staging / kLayout.binaries;
        gatherEngine(items, binaries);
        gatherGame(items, m_staging / kLayout.game);
        if (m_settings.cppRuntime) gatherCppRuntime(items, m_staging / kLayout.cppRuntime);

        uintmax_t total = 0;
        std::set<fs::path> folders;
        for (const CopyItem& item : items) {
            total += item.bytes;
            folders.insert(item.to.parent_path());
        }
        std::error_code ec;
        for (const fs::path& folder : folders) {
            fs::create_directories(folder, ec);
            if (ec) fail("Can't create " + folder.string() + ": " + ec.message());
        }
        log(LogLevel::Info,
            "Copying " + std::to_string(items.size()) + " files (" + std::to_string(total / (1024 * 1024)) + " MB)");

        // Disk-bound, so a few threads of its own: the engine's job workers stay free for
        // frames while the editor packages in the background.
        std::atomic<size_t> next{0};
        std::atomic<uintmax_t> done{0};
        std::mutex errorMutex;
        std::string error;
        const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
        auto worker = [&] {
            for (size_t i; (i = next.fetch_add(1)) < items.size();) {
                if (m_options.cancel && m_options.cancel->load()) return;
                const CopyItem& item = items[i];
                std::error_code copyError;
                if (item.link)
                    fs::copy_symlink(item.from, item.to, copyError);
                else
                    fs::copy_file(item.from, item.to, fs::copy_options::overwrite_existing, copyError);
                if (copyError) {
                    std::lock_guard lock(errorMutex);
                    if (error.empty()) error = "Can't copy " + item.from.string() + ": " + copyError.message();
                    next = items.size();
                    return;
                }
                done += item.bytes;
            }
        };
        std::vector<std::thread> pool;
        for (unsigned i = 1; i < threads; ++i) pool.emplace_back(worker);
        // This thread copies too, and reports progress between files.
        std::thread reporter;
        std::atomic<bool> copying{true};
        if (m_options.progress) {
            reporter = std::thread([&] {
                while (copying) {
                    progress(PackageStep::Copy, total ? float(double(done) / double(total)) : 1.0f);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            });
        }
        worker();
        for (std::thread& thread : pool) thread.join();
        copying = false;
        if (reporter.joinable()) reporter.join();
        if (!error.empty()) fail(error);
        checkCancel();

        if (m_settings.strip) strip(binaries);

        // The program players start, and what it needs to know.
        fs::permissions(
            binaries / m_executable,
            fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                fs::perms::others_exec,
            ec);
        writeMarker();
        writeLauncher();
        writeReadme();
        for (const auto& [path, text] : m_extraFiles) writeFile(path, text);
        m_result.files = items.size() + 3 + m_extraFiles.size();
        progress(PackageStep::Copy, 1.0f);
    }

    // Drops the symbol tables players don't need (they make up most of a Release build's
    // size); the symbols plugins are loaded by stay.
    void strip(const fs::path& binaries) {
#if defined(__linux__)
        std::string files;
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(binaries, ec)) {
            if (!entry.is_regular_file(ec) || entry.is_symlink(ec)) continue;
            if (entry.path().parent_path().filename() == "cpp-runtime") continue;   // the system's, already stripped
            if (packaging::readBinaryInfo(entry.path()).binary) files += " " + shellQuote(entry.path());
        }
        if (files.empty()) return;
        log(LogLevel::Info, "Stripping symbols");
        std::vector<std::string> lines;
        const int code = packaging::runCommand(
            "strip --strip-unneeded" + files, [&](std::string_view line) { lines.emplace_back(line); });
        if (code != 0)
            log(LogLevel::Warning,
                "Couldn't strip symbols (is binutils installed?), so the package is bigger than it needs to be" +
                    (lines.empty() ? std::string() : ": " + lines.front()));
#else
        (void)binaries;
#endif
    }

    void add(std::vector<CopyItem>& items, const fs::path& from, const fs::path& to) {
        std::error_code ec;
        CopyItem item{from, to, 0, fs::is_symlink(from, ec)};
        if (!item.link) item.bytes = fs::file_size(from, ec);
        items.push_back(std::move(item));
    }

    void addFolder(std::vector<CopyItem>& items, const fs::path& from, const fs::path& to) {
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(from, ec)) {
            if (entry.is_regular_file(ec) || entry.is_symlink(ec))
                add(items, entry.path(), to / entry.path().lexically_relative(from));
        }
    }

    // The player (renamed for the game), the engine libraries next to it, the renderer
    // plugins, this project's game module, and the shaders and fonts they load.
    void gatherEngine(std::vector<CopyItem>& items, const fs::path& to) {
        std::error_code ec;
        add(items, m_player, to / m_executable);
        for (const auto& entry : fs::directory_iterator(m_engine, ec)) {
            if ((entry.is_regular_file(ec) || entry.is_symlink(ec)) && Layout::library(entry.path()))
                add(items, entry.path(), to / entry.path().filename());
        }
        for (const char* folder : {"plugins", "shaders", "fonts"}) {
            if (fs::is_directory(m_engine / folder, ec)) addFolder(items, m_engine / folder, to / folder);
        }
        // Third-party licenses go where players see them.
        if (fs::is_directory(m_engine / "licenses", ec))
            addFolder(items, m_engine / "licenses", m_staging / "Licenses");
#if defined(__APPLE__)
        gatherMoltenVK(items, to);
#endif
        if (pluginsReloadable()) {
            const fs::path module = pluginPath("game", m_project.gameModule);
            if (!fs::exists(module, ec))
                fail(
                    "The game module " + module.filename().string() +
                    " isn't built. Build it with: cmake --build build --target game_" + m_project.gameModule);
            add(items, module, to / "modules" / module.filename());
        }
    }

#if defined(__APPLE__)
    // Macs have no Vulkan driver of their own. MoltenVK runs Vulkan on Metal; the game
    // loads it from next to itself (Platform's Window), so players need nothing installed.
    void gatherMoltenVK(std::vector<CopyItem>& items, const fs::path& to) {
        std::vector<fs::path> candidates;
        if (const char* sdk = std::getenv("VULKAN_SDK"); sdk && *sdk)
            candidates.push_back(fs::path(sdk) / "lib" / "libMoltenVK.dylib");
        candidates.emplace_back("/usr/local/lib/libMoltenVK.dylib");
        candidates.emplace_back("/opt/homebrew/lib/libMoltenVK.dylib");
        std::error_code ec;
        for (const fs::path& path : candidates) {
            if (!fs::is_regular_file(path, ec)) continue;
            add(items, fs::canonical(path, ec), to / "libMoltenVK.dylib");
            m_extraFiles.emplace_back(
                m_staging / "Licenses" / "MoltenVK-LICENSE.txt",
                "MoltenVK (libMoltenVK.dylib) is licensed under the Apache License, Version 2.0:\n"
                "https://www.apache.org/licenses/LICENSE-2.0\n"
                "Source: https://github.com/KhronosGroup/MoltenVK\n");
            return;
        }
        log(LogLevel::Warning,
            "MoltenVK wasn't found (install the Vulkan SDK), so players will need it installed to run the game");
    }
#endif

    bool excluded(const std::string& rel, const std::string& name) const {
        // steam_appid.txt makes Steam treat the folder as a development launch, and packet
        // captures hold players' traffic: neither ships, even when the project has them.
        static const char* always[] = {".*",    "Saved",           "Source",
                                       "Tools", "CMakeLists.txt",  "Config/Package.swpackage",
                                       "*.tmp", "steam_appid.txt", "*.swnetcap"};
        auto matches = [&](const std::string& pattern) {
            return pattern.find('/') != std::string::npos ? globMatch(pattern, rel) : globMatch(pattern, name);
        };
        for (const char* pattern : always)
            if (matches(pattern)) return true;
        for (const std::string& pattern : m_settings.exclude)
            if (matches(pattern)) return true;
        return false;
    }

    // The project folder minus what only its developers need: Source/, Saved/, tools,
    // hidden files and anything matching an exclude pattern.
    void gatherGame(std::vector<CopyItem>& items, const fs::path& to) {
        std::error_code ec;
        const fs::path output = m_settings.outputFolder(m_project);
        size_t content = 0;
        for (auto it = fs::recursive_directory_iterator(m_project.root, ec); !ec && it != fs::end(it);
             it.increment(ec)) {
            const fs::path rel = it->path().lexically_relative(m_project.root);
            const std::string relText = rel.generic_string();
            if (excluded(relText, it->path().filename().string()) || it->path() == output) {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(ec)) {
                add(items, it->path(), to / rel);
                ++content;
            }
        }
        if (ec) fail("Can't read the project folder " + m_project.root.string() + ": " + ec.message());
        content += gatherTextureCache(items, to);
        log(LogLevel::Info, "Game files: " + std::to_string(content));
    }

    // The compressed textures from Saved/Cache/Textures (compressTextures). Cache files are
    // named <asset>-<key>.ktx2; older keys of an asset are left behind, so only the newest
    // file of each asset ships.
    size_t gatherTextureCache(std::vector<CopyItem>& items, const fs::path& to) {
        if (m_project.textureCompression == "off") return 0;
        const fs::path cache = m_project.statePath() / "Cache" / "Textures";
        std::map<std::string, std::pair<fs::path, fs::file_time_type>> newest;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(cache, ec)) {
            const std::string name = entry.path().filename().string();
            const size_t dash = name.rfind('-');
            if (entry.path().extension() != ".ktx2" || dash == std::string::npos || !entry.is_regular_file(ec))
                continue;
            const auto written = entry.last_write_time(ec);
            auto& [file, time] = newest[name.substr(0, dash)];
            if (file.empty() || written > time) file = entry.path(), time = written;
        }
        for (const auto& [asset, file] : newest)
            add(items, file.first, to / file.first.lexically_relative(m_project.root));
        return newest.size();
    }

    // This machine's libstdc++ and libgcc, which the launcher uses only where the
    // system's own are too old for the game.
    void gatherCppRuntime(std::vector<CopyItem>& items, const fs::path& to) {
#if defined(__linux__)
        std::vector<fs::path> loaded;
        dl_iterate_phdr(
            [](dl_phdr_info* info, size_t, void* data) {
                if (info->dlpi_name && *info->dlpi_name)
                    static_cast<std::vector<fs::path>*>(data)->emplace_back(info->dlpi_name);
                return 0;
            },
            &loaded);
        for (const char* name : {"libstdc++.so.6", "libgcc_s.so.1"}) {
            for (const fs::path& path : loaded) {
                if (path.filename() != name) continue;
                std::error_code ec;
                const fs::path real = fs::canonical(path, ec);
                if (!ec) add(items, real, to / name);
                break;
            }
        }
#else
        (void)items, (void)to;
#endif
    }

    std::string quoted(const std::string& text) const {
        std::ostringstream out;
        out << std::quoted(text);
        return out.str();
    }

    void writeFile(const fs::path& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!(out << text) || !out.flush()) fail("Can't write " + path.string());
    }

    // Binaries/shockwave-package.txt: tells the player which project to run, and tells
    // people and tools what this package is.
    void writeMarker() {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm utc{};
#if defined(_WIN32)
        gmtime_s(&utc, &now);
#else
        gmtime_r(&now, &utc);
#endif
        std::ostringstream text;
        text << "# Written by Shockwave's packager. The player in this folder runs this project.\n"
             << "shockwave_package_info 1\n"
             << "project " << quoted("../" + std::string(kLayout.game) + "/" + m_project.file.filename().string())
             << "\n"
             << "name " << quoted(m_settings.displayName(m_project)) << "\n"
             << "version " << quoted(m_settings.version) << "\n"
             << "platform " << quoted(packagePlatform()) << "\n"
             << "engine " << quoted(SHOCKWAVE_VERSION) << "\n"
             << "build_type " << quoted(SHOCKWAVE_BUILD_CONFIG) << "\n"
             << "created " << quoted((std::ostringstream() << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ")).str()) << "\n";
        if (!m_result.minimumGlibc.empty()) text << "minimum_glibc " << quoted(m_result.minimumGlibc) << "\n";
        if (!m_result.minimumCppRuntime.empty())
            text << "minimum_cpp_runtime " << quoted("GLIBCXX_" + m_result.minimumCppRuntime) << "\n";
        writeFile(m_staging / kLayout.binaries / kMarker, text.str());
    }

    void writeLauncher() {
#if defined(_WIN32)
        // Players start Binaries/<game>.exe directly for now.
        m_result.launcher = m_final / kLayout.binaries / m_executable;
#else
        const fs::path path = m_staging / m_executable;
        std::ostringstream script;
        script << "#!/bin/sh\n"
               << "# Starts " << m_settings.displayName(m_project) << ". Options go to the game; try --help.\n"
#if defined(__APPLE__)
               << "here=$(cd \"$(dirname \"$0\")\" && pwd -P)\n"
               << "exec \"$here/" << kLayout.binaries << "/" << m_executable << "\" \"$@\"\n";
#else
               << "here=$(dirname \"$(readlink -f \"$0\")\")\n"
               << "bin=\"$here/" << kLayout.binaries << "\"\n"
               << "# The C++ runtime in " << kLayout.cppRuntime << " is used only when this system's own\n"
               << "# is too old for the game.\n"
               << "need=@CPP_RUNTIME@\n"
               << "if [ -n \"$need\" ] && [ -f \"$here/" << kLayout.cppRuntime << "/libstdc++.so.6\" ]; then\n"
               << "    system=$( (ldconfig -p || /sbin/ldconfig -p) 2>/dev/null | grep -m1 'libstdc++\\.so\\.6 .*"
#if defined(__aarch64__)
               << "AArch64"
#else
               << "x86-64"
#endif
               << "' | sed 's/.*=> //')\n"
               << "    if [ -z \"$system\" ] || ! grep -q \"$need\" \"$system\" 2>/dev/null; then\n"
               << "        export LD_LIBRARY_PATH=\"$here/" << kLayout.cppRuntime
               << "${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"\n"
               << "    fi\n"
               << "fi\n"
               << "exec \"$bin/" << m_executable << "\" \"$@\"\n";
#endif
        m_launcherText = script.str();
        writeFile(path, m_launcherText);
        std::error_code ec;
        fs::permissions(
            path, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
        m_result.launcher = m_final / m_executable;
#endif
    }

    std::string requirements() const {
        std::ostringstream text;
#if defined(__linux__)
        text << "- 64-bit Linux";
        if (!m_result.minimumGlibc.empty()) text << " with glibc " << m_result.minimumGlibc << " or newer";
        text << ". Any distribution works.\n";
#endif
#if defined(__APPLE__)
        // The macOS the build targets: the building Mac's own unless CMAKE_OSX_DEPLOYMENT_TARGET says older.
        constexpr int macos = __MAC_OS_X_VERSION_MIN_REQUIRED;
        text << "- macOS " << macos / 10000 << "." << macos / 100 % 100
             << " or newer. Graphics run on Metal through MoltenVK, which ships with the game.\n";
#else
        text << "- A graphics driver with Vulkan 1.3: Mesa 23 or newer for AMD and Intel, or NVIDIA's\n"
             << "  driver 525 or newer.\n";
#endif
        return text.str();
    }

    void writeReadme() {
        std::ostringstream text;
        text << m_settings.displayName(m_project) << " " << m_settings.version << "\n\n"
             << "To play, run " << m_executable << " in this folder: double-click it, or type\n"
             << "./" << m_executable << " in a terminal opened here.\n\n"
             << "What it needs:\n"
             << "@REQUIREMENTS@\n"
             << "\n"
             << "Options go after the program name, like ./" << m_executable << " --fps 144:\n"
             << "  --fps N           cap the frame rate (0 for no cap)\n"
             << "  --vsync on|off    sync frames to the screen\n"
             << "  --size WxH        window size, like 1920x1080\n"
             << "  --help            every option\n\n"
             << steamReadme() << "Made with the Shockwave engine.\n";
        m_readmeText = text.str();
        writeFile(m_staging / "README.txt", m_readmeText);
    }

    std::string steamReadme() const {
        if (!m_options.steam.uses) return {};
        if (!m_steamEngine) return "This build plays offline only.\n\n";
        return std::string("Online play needs Steam running and signed in; without it the game plays offline.\n") +
               "The Steam runtime (" + steamRuntimeLibrary() +
               ") is Valve Corporation's, redistributed under the Steamworks SDK Access Agreement.\n\n";
    }

    // ---- Verify -------------------------------------------------------------------------
    // The package must stand on its own: every library it links against is inside it or
    // on every system, it never looks for them in this machine's build folder, and its
    // own player passes --check.
    void verify() {
        progress(PackageStep::Verify, 0.0f);
        const fs::path binaries = m_staging / kLayout.binaries;
        std::set<std::string> shipped;
        std::vector<fs::path> files;
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(binaries, ec)) {
            if (entry.is_regular_file(ec) || entry.is_symlink(ec)) shipped.insert(entry.path().filename().string());
            if (entry.is_regular_file(ec) && !entry.is_symlink(ec)) files.push_back(entry.path());
        }
        const fs::path cppRuntime = m_staging / kLayout.cppRuntime;
        std::set<std::string> missing;
        for (const fs::path& file : files) {
            const packaging::BinaryInfo info = packaging::readBinaryInfo(file);
            if (!info.binary) continue;
            const std::string shown = file.lexically_relative(m_staging).generic_string();
            for (const std::string& path : info.searchPaths) {
                if (path.rfind("$ORIGIN", 0) != 0)
                    log(LogLevel::Warning, shown + " also looks for libraries in " + path +
                                               ", which players won't have (harmless "
                                               "as long as everything it needs is in the package)");
            }
            for (const std::string& library : info.needed) {
                if (!shipped.count(library) && !systemLibrary(library) && missing.insert(library).second)
                    log(LogLevel::Warning,
                        shown + " needs " + library + ", which isn't in the package; players need it installed");
            }
            if (packaging::compareVersions(info.glibc, m_result.minimumGlibc) > 0) m_result.minimumGlibc = info.glibc;
            const bool runtimeItself = file.parent_path() == cppRuntime;
            if (!runtimeItself && packaging::compareVersions(info.glibcxx, m_result.minimumCppRuntime) > 0)
                m_result.minimumCppRuntime = info.glibcxx;
        }
        // Steam: the runtime the player links ships next to it; nothing that would make it a
        // development launch, or anyone's captured traffic, ships at all.
        if (m_steamEngine && !shipped.count(steamRuntimeLibrary()))
            fail(std::string("The package is missing the Steam runtime ") + steamRuntimeLibrary());
        for (const auto& entry : fs::recursive_directory_iterator(m_staging, ec)) {
            const std::string name = entry.path().filename().string();
            if (name == "steam_appid.txt" || entry.path().extension() == ".swnetcap")
                fail(
                    "The package would ship " + entry.path().lexically_relative(m_staging).generic_string() +
                    ", which never goes to players");
        }
        if (m_steamEngine) log(LogLevel::Info, std::string("Steam runtime: ") + steamRuntimeLibrary());
        progress(PackageStep::Verify, 0.3f);

        // What the oldest system it runs on needs, now that it's known.
        std::string requirementsText = requirements();
        if (!m_result.minimumCppRuntime.empty()) {
            const std::string gcc = packaging::gccForGlibcxx(m_result.minimumCppRuntime);
            const bool shipsRuntime = fs::exists(cppRuntime / "libstdc++.so.6", ec);
            log(LogLevel::Info, "Players need glibc " + m_result.minimumGlibc + " or newer" +
                                    (shipsRuntime ? std::string(" (the C++ runtime ships with the game)")
                                                  : ", and the C++ runtime from GCC " + (gcc.empty() ? "?" : gcc) +
                                                        " or newer (GLIBCXX_" + m_result.minimumCppRuntime + ")"));
            if (!shipsRuntime)
                requirementsText +=
                    "- The C++ runtime (libstdc++) from GCC " + (gcc.empty() ? "?" : gcc) + " or newer.\n";
        }
        replace(m_staging / "README.txt", m_readmeText, "@REQUIREMENTS@\n", requirementsText);
        if (!m_launcherText.empty())
            replace(
                m_staging / m_executable, m_launcherText, "@CPP_RUNTIME@",
                m_result.minimumCppRuntime.empty() ? std::string("\"\"") : "GLIBCXX_" + m_result.minimumCppRuntime);
        writeMarker();
        checkCancel();

        // Run it: the packaged player finds its project and code from inside the folder.
        log(LogLevel::Info, "Running the packaged game's own checks");
        std::vector<std::string> lines;
        const int code = packaging::runCommand(
            headless() + shellQuote(binaries / m_executable) + " --check",
            [&](std::string_view line) { lines.emplace_back(line); });
        if (code != 0) {
            for (const std::string& line : lines) log(LogLevel::Error, line);
            fail("The packaged game didn't pass its own checks (exit code " + std::to_string(code) + ")");
        }
        checkCancel();
        progress(PackageStep::Verify, 1.0f);
    }

    void replace(const fs::path& path, std::string& text, const std::string& from, const std::string& to) {
        const size_t at = text.find(from);
        if (at == std::string::npos) return;
        text.replace(at, from.size(), to);
        std::error_code ec;
        const auto perms = fs::status(path, ec).permissions();
        writeFile(path, text);
        fs::permissions(path, perms, ec);
    }

    // ---- Archive ------------------------------------------------------------------------
    void finish() {
        progress(PackageStep::Archive, 0.0f);
        std::error_code ec;
        // Only now does the new package replace the old one.
        fs::remove_all(m_final, ec);
        if (ec) fail("Can't remove the previous package " + m_final.string() + ": " + ec.message());
        fs::rename(m_staging, m_final, ec);
        if (ec) fail("Can't move the package into place: " + ec.message());
        m_staging.clear();
        m_result.folder = m_final;
        for (const auto& entry : fs::recursive_directory_iterator(m_final, ec))
            if (entry.is_regular_file(ec) && !entry.is_symlink(ec)) m_result.bytes += entry.file_size(ec);

        const fs::path archive = m_final.string() + ".tar.gz";
        if (!m_settings.archive) fs::remove(archive, ec);   // it would be an older build
        if (m_settings.archive) {
#if defined(_WIN32)
            log(LogLevel::Warning, "Archives aren't made on Windows yet; zip the folder by hand");
#else
            const fs::path partial = archive.string() + ".partial";
            log(LogLevel::Info, "Archiving to " + archive.filename().string());
            const int code =
                run("tar -czf " + shellQuote(partial) + " -C " + shellQuote(m_final.parent_path()) + " " +
                        shellQuote(m_final.filename()),
                    LogLevel::Warning);
            if (code != 0) {
                fs::remove(partial, ec);
                fail("Making the archive failed (tar exit code " + std::to_string(code) + "); the folder is ready");
            }
            fs::rename(partial, archive, ec);
            if (ec) fail("Can't move the archive into place: " + ec.message());
            m_result.archive = archive;
#endif
        }
        progress(PackageStep::Archive, 1.0f);
        std::ostringstream summary;
        summary << "Packaged " << m_final.string() << " (" << std::fixed << std::setprecision(1)
                << double(m_result.bytes) / (1024.0 * 1024.0) << " MB)";
        log(LogLevel::Info, summary.str());
        progress(PackageStep::Done, 1.0f);
    }

    const Project& m_project;
    const PackageSettings& m_settings;
    const PackageOptions& m_options;
    PackageResult m_result;
    fs::path m_engine, m_player, m_final, m_staging;
    std::string m_executable, m_launcherText, m_readmeText;
    std::vector<std::pair<fs::path, std::string>> m_extraFiles;   // written with the copy
    bool m_steamEngine = false;   // the engine was built with Steamworks
};

}   // namespace

PackageResult packageProject(const Project& project, const PackageSettings& settings, const PackageOptions& options) {
    return Packager(project, settings, options).run();
}

const char* steamRuntimeLibrary() {
#if defined(_WIN32)
    return "steam_api64.dll";
#elif defined(__APPLE__)
    return "libsteam_api.dylib";
#else
    return "libsteam_api.so";
#endif
}

std::optional<fs::path> packagedProject() {
    static const std::optional<fs::path> found = []() -> std::optional<fs::path> {
        const fs::path marker = executableDir() / kMarker;
        std::ifstream in(marker);
        if (!in) return std::nullopt;
        for (std::string line; std::getline(in, line);) {
            std::istringstream row(line);
            std::string key, value;
            if (row >> key && key == "project" && row >> std::quoted(value) && !value.empty())
                return (executableDir() / value).lexically_normal();
        }
        return std::nullopt;
    }();
    return found;
}

}   // namespace shockwave
