#pragma once

// Install records (S13, docs/Release.md#platforms): what `player --verify-install` saw when
// it installed a package on one named machine and played it there, kept as evidence for
// the release's platform gates.
//
//   shockwave_install 1
//   machine "Yianni's desktop"
//   recorded "2026-11-15T10:00:00Z"
//   platform linux-x86_64
//   os "Linux 6.8.0-45-generic"
//   distribution "Ubuntu 24.04.1 LTS"
//   ...
//   package_sha256 3f2a...
//   step "install" passed 0.41 "copied 812 files to a read-only folder ..."
//   step "scenario full-run.json" passed 38.2 "48 steps"
//   result passed
//
// A gate passes on a record from an optimised release build, of the game as it is now,
// played with a real GPU, where every step passed, an update included. Nothing is ticked
// by hand. Reading is strict (unknown keys are errors with file:line) and changes nothing
// on failure; writing goes through a temporary file.

#include <Projects/Diagnostic.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

struct Project;

enum class InstallStepResult : int { Passed, Failed, Skipped };
const char* installStepResultName(InstallStepResult result);   // "passed", "failed", "skipped"

// One thing the verifier did: "install", "check", "scenario <file>", "user data", "update",
// "reinstall".
struct InstallStep {
    std::string name;
    InstallStepResult result = InstallStepResult::Passed;
    double seconds = 0;
    std::string detail;   // what it showed, or why it failed
    bool operator==(const InstallStep&) const = default;
};

struct InstallRecord {
    // The machine: its name as people know it, and what it reported.
    std::string machine;
    std::string recorded;   // UTC, ISO 8601
    std::string platform;   // the package's: "linux-x86_64"
    std::string os;   // "Linux 6.8.0-45-generic"
    std::string distribution;   // "Ubuntu 24.04.1 LTS"; "" where there's no such thing
    std::string glibc;   // the system's C library version (Linux); "" elsewhere
    std::string cpu;
    // How the game drew, as its own report said.
    std::string renderer;   // "vulkan", "null"
    std::string gpu;
    std::string gpuKind;   // "dedicated", "integrated", "virtual", "software", "other", "none"
    std::string gpuDriver;
    // The package: its folder name, what its marker says, and exact hashes of what was tested.
    std::string package;
    std::string version;
    std::string buildConfig;   // "Release", ...
    std::string buildId, contentId;   // a release's cook; "" for a development package
    std::string packageSha256;   // every file's path and bytes, in path order (installPackageDigest)
    std::string archiveSha256;   // the .tar.gz beside it, "" when there was none
    uint64_t files = 0, bytes = 0;
    // How it was installed: read-only, at a path with spaces and non-ASCII letters, started
    // from another folder, with only a minimal environment (no LD_LIBRARY_PATH, a fresh home).
    bool readOnly = false, spacesInPath = false, unicodePath = false, otherFolder = false, cleanEnvironment = false;
    std::vector<InstallStep> steps;
    bool passed = false;   // every step passed (result passed)

    // The step named so, or null.
    const InstallStep* step(std::string_view name) const;
    bool operator==(const InstallRecord&) const = default;
};

// ---- Files ------------------------------------------------------------------------------

// Release/Installs under the project, where records live.
std::filesystem::path installFolder(const Project& project);
// A file name for a machine and platform: "yiannis-desktop-linux-x86_64.swinstall".
std::string installFileName(std::string_view machine, std::string_view platform);

std::string installText(const InstallRecord& record);
// The first problem stops it, into `error` with `shown` and the line. False leaves `out`.
bool parseInstall(std::string_view text, std::string_view shown, InstallRecord& out, Diagnostic& error);
bool readInstall(const std::filesystem::path& file, std::string_view shown, InstallRecord& out, Diagnostic& error);
bool writeInstall(const std::filesystem::path& file, const InstallRecord& record, std::string& error);

struct InstallRecords {
    std::vector<InstallRecord> records;
    std::vector<std::string> files;   // project-relative, one per record
    std::vector<Diagnostic> problems;   // records that don't read (InstallUnreadable)
};
// Every .swinstall in the project's Release/Installs, sorted by file name.
InstallRecords readInstallRecords(const Project& project);

// ---- The gate ---------------------------------------------------------------------------

// The steps a record needs, all passed, before it qualifies a platform. Part 2 of S13
// verifies updates and reinstalls; until a record has them, the gate stays unverified.
std::span<const char* const> requiredInstallSteps();   // "install", "check", "user data", "update", "reinstall"

// Why one record doesn't qualify its platform: every problem, in a stable order; empty
// when it does. `currentBuildId` is the game's build now ("" unknown): another build's
// record is stale.
std::vector<std::string> installProblems(const InstallRecord& record, std::string_view currentBuildId);

enum class InstallVerdict { Passed, Failed, Unverified };
struct InstallGate {
    InstallVerdict verdict = InstallVerdict::Unverified;
    std::string reason;
    int record = -1;   // the record it rests on, or -1
};
// The gate for one platform: Passed when a record qualifies it. Failed when the newest
// record of the current build for that platform failed a step. Unverified otherwise: no
// record, only stale ones, or ones that don't prove enough (the null renderer, no update).
InstallGate installGate(
    std::string_view platform, std::span<const InstallRecord> records, std::span<const std::string> files,
    std::string_view currentBuildId);

}   // namespace shockwave
