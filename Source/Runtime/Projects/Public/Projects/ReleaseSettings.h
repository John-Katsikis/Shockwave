#pragma once

// The release contract: Config/Release.swrelease, what a project promises to ship
// (docs/Release.md, docs/ReleaseSchema.md). It names the game for good (application_id),
// its version, the platforms and modes it's qualified for, player counts, frame and
// memory budgets, and the scenario that proves it can be played from start to finish.
//
//   shockwave_release 1
//   application_id "com.example.mygame"
//   release_version "1.0.0"
//   platform linux-x86_64
//   platform windows-x86_64
//   mode offline
//   max_players 1
//   target_fps 60
//   ...
//
// A project without the file is a development project: everything works as before, and
// release qualification reports that there's no contract. One key per line (platform and
// mode repeat), `#` starts a comment. Every key is required: a release doesn't get to
// inherit defaults nobody chose. Unknown or repeated keys, values out of range and
// non-finite numbers are errors with file:line; nothing is clamped. Reading is
// transactional (a failed read changes nothing); writing goes through a temporary file.
//
// One descriptor table (releaseFields) drives the parser, the writer, the validator, the
// editor's Project > Release readiness window and the generated docs/ReleaseSchema.md.

#include <Projects/Diagnostic.h>
#include <Projects/LevelCatalog.h>
#include <Projects/Performance.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace shockwave {

struct Project;

enum class ReleaseMode : int { Offline, Online };
enum class ReleaseGraphicsTier : int { Low, Medium, High, Ultra };

struct ReleaseSettings {
    std::string applicationId;   // "com.example.mygame": stable for the game's life
    std::string releaseVersion = "1.0.0";
    std::vector<std::string> platforms;   // "linux-x86_64", "windows-x86_64", ...
    std::vector<ReleaseMode> modes;
    uint32_t maxPlayers = 1;
    uint32_t targetFps = 60;
    uint32_t simulationHz = 60;
    ReleaseGraphicsTier graphicsTier = ReleaseGraphicsTier::Medium;
    uint32_t cpuBudgetMib = 4096;
    uint32_t gpuBudgetMib = 2048;
    uint32_t sharedBudgetMib = 0;   // 0: CPU and GPU memory are separate (a discrete GPU)
    uint32_t loadingBudgetMib = 512;
    double transitionTimeoutSeconds = 30;
    std::string acceptanceScenario;   // project-relative .json flow-test scenario
    // Performance budgets (contract version 2, S10, docs/Performance.md): frame-time
    // percentiles and the worst frame of play, and how long one level change may take. All
    // 0 means a version 1 contract that doesn't state them yet: it still reads, and the
    // performance gate says what's missing. They're set together or not at all.
    double frameP95Ms = 0;
    double frameP99Ms = 0;
    double maxHitchMs = 0;
    double levelLoadSeconds = 0;

    bool hasMode(ReleaseMode mode) const;
    bool hasPerformanceBudgets() const {
        return frameP95Ms > 0 || frameP99Ms > 0 || maxHitchMs > 0 || levelLoadSeconds > 0;
    }
    // The frame budget: 1000 / target_fps ms (16.667 at 60, 8.333 at 120).
    double frameBudgetMs() const { return targetFps > 0 ? 1000.0 / targetFps : 0; }
    // Fills the budgets from target_fps: p95 at the frame budget, p99 at 1.25x, the worst
    // frame at 3x, a level change in 10 s (or the transition timeout, if shorter). For the
    // editor's "Add budgets" button: a starting point someone then chooses to keep.
    void suggestPerformanceBudgets();
    bool operator==(const ReleaseSettings&) const = default;
};

// ---- The descriptor table ----------------------------------------------------------------

enum class ReleaseFieldKind { Id, Version, Platforms, Modes, Integer, Seconds, Milliseconds, Choice, Path };

struct ReleaseField {
    const char* key;
    ReleaseFieldKind kind;
    std::variant<
        std::string ReleaseSettings::*, std::vector<std::string> ReleaseSettings::*,
        std::vector<ReleaseMode> ReleaseSettings::*, uint32_t ReleaseSettings::*, double ReleaseSettings::*,
        ReleaseGraphicsTier ReleaseSettings::*>
        member;
    double min = 0, max = 0;   // Integer, Seconds and Milliseconds
    std::span<const char* const> choices;   // Platforms, Modes and Choice: names, in enum order
    const char* label;   // for people: the editor's row
    const char* meaning;   // one line for the editor's tooltip and the schema
    int since = 1;   // the contract version that added it; required from then on
};

// Every key, in file order.
std::span<const ReleaseField> releaseFields();
const ReleaseField* findReleaseField(std::string_view key);
// The platforms a contract can name, as packages name them.
std::span<const char* const> releasePlatformNames();
const char* releaseModeName(ReleaseMode mode);
const char* releaseGraphicsTierName(ReleaseGraphicsTier tier);

// A field's values as file text, one entry per line it takes (platform and mode may take
// several, or none when empty): {"\"com.example.mygame\""}, {"linux-x86_64", "windows-x86_64"}.
// A version 2 budget left at 0 has none.
std::vector<std::string> releaseFieldText(const ReleaseSettings& settings, const ReleaseField& field);

// ---- Files ------------------------------------------------------------------------------

// A contract as read: the values, and the first line each key was on (0 if left out).
struct ReleaseFile {
    ReleaseSettings settings;
    std::vector<int> lines;   // one per releaseFields() entry
    int line(std::string_view key) const;
};

// Parses the text of a .swrelease. The first syntax or range error stops it and goes into
// `error` (file and line set from `shown`); rules between fields are validateRelease's job.
// False leaves `out` unchanged.
bool parseRelease(std::string_view text, std::string_view shown, ReleaseFile& out, Diagnostic& error);
bool readRelease(const std::filesystem::path& file, std::string_view shown, ReleaseFile& out, Diagnostic& error);
// The file text, every key in table order, so a write followed by a read is exact.
std::string releaseText(const ReleaseSettings& settings);
// Refuses settings with errors of their own (validateRelease without inputs); writes
// through a temporary file and renames it.
bool writeRelease(const std::filesystem::path& file, const ReleaseSettings& settings, std::string& error);

// Config/Release.swrelease under the project root.
std::filesystem::path releasePath(const Project& project);

// The ID an installed game's per-user folders are named by (docs/UserPaths.md): the
// contract's application_id, or, without a readable contract, "shockwave.<game module>"
// (lowercase). Never the display name, so renaming the game doesn't move its players' data.
std::string fallbackApplicationId(const Project& project);
std::string projectApplicationId(const Project& project);

// ---- Validation -------------------------------------------------------------------------

// What the rest of the project says, for the rules a contract shares with other settings.
// The caller reads these (Player and the editor do), so Projects depends on nothing else.
struct ReleaseInputs {
    double projectTickRate = 0;   // 0: not checked
    std::string packageFile;   // "Config/Package.swpackage", for messages
    std::string packageVersion;   // the version packages get; empty: not checked
    // Config/Network.swnetwork
    std::string networkFile = "Config/Network.swnetwork";
    bool networkPresent = false;
    std::string networkError;   // the file exists but doesn't read
    bool networkEnabled = false;
    bool networkSteam = false;   // plays over Steam (the steam transport)
    uint32_t networkMaxPlayers = 0;
    uint32_t steamAppId = 0;
    // The project's default graphics memory budget (graphics_memory_budget_mb, 0 = none).
    uint32_t graphicsMemoryBudgetMb = 0;
    // The level catalog (S02, docs/Levels.md). Checked: it was read and its files checked
    // (levelReleaseInputs fills these). Links checked: every level's
    // scene was loaded and the exits and spawns between them followed too.
    bool levelCatalogChecked = false;
    bool levelLinksChecked = false;
    bool levelCatalogImplicit = true;   // no catalog: the startup scene is the only level
    size_t levelCount = 0;
    std::string levelEntry;
    std::string levelCatalogProblem;   // the first error, or empty
    // Whether the acceptance scenario's file exists; unknown when not asked.
    bool acceptanceScenarioExists = false;
    // The build this engine is ("Release", "Debug", ... from CMake), for the build gate.
    std::string buildConfig;
    // Performance records (S10, Projects/Performance.h): checked means the project's
    // Release/Performance folder was read. currentBuildId is the game's cook build_id now,
    // "" unknown; a record from another build is stale.
    bool performanceChecked = false;
    std::vector<PerformanceRecord> performanceRecords;
    std::vector<std::string> performanceFiles;
    std::vector<Diagnostic> performanceProblems;
    std::string currentBuildId;
    // The graphics release tier (S10, Engine/ReleaseTier.h, docs/ReleaseTier.md). Checked:
    // Config/ReleaseTier.swtier was looked for and, when present, read and compared with the
    // presets and the project's default settings (releaseTierInputs fills these).
    bool tierChecked = false;
    bool tierPresent = false;
    std::string tierFile = "Config/ReleaseTier.swtier";
    std::string tierSummary;   // "fallback low; 2 mandatory, ..."
    int tierFallback = -1;   // the fallback preset, 0 low .. 3 ultra; -1 unknown
    std::vector<Diagnostic> tierProblems;
    // Where the installed game writes (S01, docs/UserPaths.md): the per-user root its
    // application ID resolves to on this machine, or why it doesn't. Unchecked leaves the
    // user_paths gate unverified.
    bool userPathsChecked = false;
    std::string userPathsApplicationId;
    std::string userPathsRoot;
    std::string userPathsProblem;
    // Crash recovery (S12, docs/Diagnostics.md): checked means the game's crash reports were
    // looked for (Saved/Crashes from source, the installed per-user Crashes folder). A report
    // is resolved by moving it into Crashes/Resolved (the editor's Release readiness does).
    bool crashChecked = false;
    bool crashCaptureSupported = false;   // this platform writes crash reports
    bool crashSymbolsKept = true;   // packages keep their symbols (keep_symbols, or strip false)
    std::vector<std::string> unresolvedCrashes;   // "player-....crash.txt SIGSEGV (11)", newest first
    std::string uncleanSession;   // the last player run didn't end cleanly without a report: why
};

// Fills the level_catalog inputs: reads the project's catalog and checks its files, and
// with `facts` (each level's scene, read by the caller) the links between levels too.
void levelReleaseInputs(
    const Project& project, ReleaseInputs& inputs, const std::map<std::string, LevelSceneFacts>* facts = nullptr);

// Everything wrong with a contract: its own rules, then (with inputs) the rules it shares
// with the project, package and network settings. `shown` names the file in diagnostics;
// `file` (optional) adds lines.
void validateRelease(
    const ReleaseSettings& settings, const ReleaseInputs* inputs, std::string_view shown, const ReleaseFile* file,
    std::vector<Diagnostic>& diagnostics);

// ---- Qualification ----------------------------------------------------------------------

// Each gate a release must pass, worked out by the engine from files and evidence it can
// measure, never ticked by hand. Gates the engine can't measure yet stay Unverified, with
// the batch (docs/ShippingImplementation.md) that brings their evidence.
enum class GateStatus { Passed, Failed, Unverified, NotApplicable };
const char* gateStatusName(GateStatus status);   // "passed", "failed", "unverified", "not_applicable"

struct ReleaseGate {
    std::string id;   // stable: "release_contract", "platform:linux-x86_64", ...
    GateStatus status = GateStatus::Unverified;
    std::string batch;   // the shipping batch that owns it ("S00")
    std::string reason;   // why it has this status, for people
};

struct ReleaseReport {
    bool present = false;   // the project has a Config/Release.swrelease
    bool readable = false;   // and it parsed
    std::string file;   // as the project names it
    ReleaseSettings settings;   // when readable
    std::vector<Diagnostic> diagnostics;
    std::vector<ReleaseGate> gates;
    std::string buildConfig;   // the engine build's configuration (ReleaseInputs::buildConfig)
    bool qualified() const;   // every gate that applies passed
};

// Reads the project's contract, validates it against the inputs and works out the gates.
// A project without a contract is reported as such (present false), not as an error.
ReleaseReport releaseReport(const Project& project, const ReleaseInputs& inputs);
// The gates for an already validated contract (report.settings and report.diagnostics).
void releaseGates(ReleaseReport& report, const ReleaseInputs& inputs);

// `player --release-info` as text and as JSON (format 1).
std::string releaseReportText(const ReleaseReport& report);
std::string releaseReportJson(const ReleaseReport& report, std::string_view projectName);

// docs/ReleaseSchema.md: the keys, the gates and the diagnostic codes.
std::string releaseSchema();

}   // namespace shockwave
