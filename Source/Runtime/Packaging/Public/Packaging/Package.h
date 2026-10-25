#pragma once

// Packaging: turns a project into a game folder players can run without the engine's
// source, the editor or a build folder (docs/Packaging.md).
//
//   ShockwaveSample-1.0.0-linux-x86_64/
//     ShockwaveSample         starts the game (a small launcher script on Linux)
//     README.txt              how to run it and what it needs
//     Binaries/               the player, renamed, with the engine's libraries,
//                             renderer plugins, the game module, shaders and fonts
//     Game/                   the project: its .swproject, Content/, Scenes/, Config/
//     Licenses/               the engine's and its libraries' licenses
//
// How a project is packaged lives in Config/Package.swpackage, a small text file that
// people edit in the editor (Build > Package game) and tools edit as text. The same
// packageProject() runs behind `player --package` and the editor's window.

#include <Core/Log.h>
#include <Projects/Project.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

SW_LOG_CATEGORY(LogPackage);

// Config/Package.swpackage. Every key is optional; a project without the file packages
// with these defaults.
struct PackageSettings {
    std::string name;   // what players see; empty means the project's name
    std::string executable;   // the program's file name; empty means the name without spaces
    std::string version = "1.0.0";
    std::filesystem::path output;   // the folder packages go in; empty means <project>/Saved/Packages
    std::vector<std::string> exclude;   // extra file patterns to leave out ("*.blend", "Content/Raw")
    bool build = true;   // build the player and game module first, so the package is never stale
    bool archive = true;   // also make a .tar.gz of the folder, ready to upload or send
    bool strip = true;   // drop symbol tables from the program and libraries (much smaller)
    // Ship this machine's C++ runtime too, used only on systems whose own is too old to
    // run the game. Lets a game built on a rolling distro run on older LTS ones.
    bool cppRuntime = true;

    // The values actually used, with the defaults filled in.
    std::string displayName(const Project& project) const;
    std::string executableName(const Project& project) const;
    std::filesystem::path outputFolder(const Project& project) const;
    bool operator==(const PackageSettings&) const = default;
};

// Where a project's package settings live: <project>/Config/Package.swpackage.
std::filesystem::path packageSettingsPath(const Project& project);
// Reads the settings file. A missing file isn't an error: settings stay at the defaults.
// Failure leaves `settings` unchanged and says which line is wrong.
bool loadPackageSettings(const Project& project, PackageSettings& settings, std::string& error);
// Writes the settings file (through a temporary file, like saveProject).
bool savePackageSettings(const Project& project, const PackageSettings& settings, std::string& error);
// Problems a package with these settings would have (bad names, an output folder inside
// Content). Empty when it's fine. `player --check` reports these too.
std::vector<std::string> validatePackageSettings(const Project& project, const PackageSettings& settings);

// The platform packages are made for: the one the engine was built for, "linux-x86_64".
const char* packagePlatform();
// The package's folder name: "<executable>-<version>-<platform>".
std::string packageFolderName(const Project& project, const PackageSettings& settings);

// The steps of packageProject, in order.
enum class PackageStep { Build, Check, Copy, Verify, Archive, Done };
inline constexpr int kPackageStepCount = 5;   // Build..Archive
const char* packageStepName(PackageStep step);

// How the project uses Steam, which the application reads from Config/Network.swnetwork
// (Packaging doesn't read network settings itself). A Steam-enabled engine ships Valve's
// runtime library (libsteam_api), never the SDK, steam_appid.txt or packet captures.
struct PackageSteam {
    bool uses = false;   // multiplayer is on and plays over Steam
    uint32_t appId = 0;   // steam_app_id
    // A development package: AppID 480 (Valve's shared test app) is allowed, with a
    // warning. Never saved in the settings file, so a release can't ship as one by mistake.
    bool development = false;
};

struct PackageOptions {
    // Where the engine that gets shipped lives: the folder with the player and its
    // libraries. Defaults to the running program's folder (build/bin).
    std::filesystem::path engineFolder;
    // The CMake build folder to build in when settings.build is on. Defaults to the one
    // this engine was built in, if it still exists; empty skips building.
    std::optional<std::filesystem::path> buildFolder;
    // Output lines as they happen, from the packaging thread.
    std::function<void(LogLevel, std::string_view)> log;
    // The step running now and how far through it is (0..1), from the packaging thread.
    std::function<void(PackageStep, float)> progress;
    // Set from any thread to stop at the next file; the half-made package is removed.
    const std::atomic<bool>* cancel = nullptr;
    PackageSteam steam;
};

struct PackageResult {
    bool ok = false;
    std::string error;   // why it failed, when it did
    std::vector<std::string> warnings;   // worth reading even when it worked
    std::filesystem::path folder;   // the finished package
    std::filesystem::path archive;   // its .tar.gz, when settings.archive is on
    std::filesystem::path launcher;   // the program players start
    size_t files = 0;
    uintmax_t bytes = 0;   // the folder's size
    double seconds = 0;
    // The oldest system the package runs on (Linux): glibc and the C++ runtime it needs.
    std::string minimumGlibc, minimumCppRuntime;
};

// The Steam runtime library's file name on this platform ("libsteam_api.so"); a
// Steam-enabled engine has it next to the player.
const char* steamRuntimeLibrary();

// Packages the project: optionally builds, runs the project checks, copies the engine
// and the project into a new folder next to any older package, verifies that folder runs
// on its own (its libraries resolve inside it and its own player --check passes), then
// swaps it in and archives it. Blocks; run it on a thread of its own. A failed or
// cancelled package leaves any previous one untouched.
PackageResult packageProject(const Project& project, const PackageSettings& settings, const PackageOptions& options);

// In a packaged game, the project it ships with (from Binaries/shockwave-package.txt),
// so the player runs that one wherever it's started from. Nothing in a normal build.
std::optional<std::filesystem::path> packagedProject();

}   // namespace shockwave
