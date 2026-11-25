// The player's command-line flags as a table (E2, AGENTS.md "Player command line"): one row
// per flag, with its names, how many values it takes and what it does. main() finds the row
// for each word and calls it; anything that isn't a flag is a project, a scene or a save.
#pragma once

#include "Player.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace player {

// What the flags set, read by main() after parsing. These were main()'s own locals.
struct ParseState {
    std::filesystem::path manifest;
    std::filesystem::path newProjectFolder;
    std::string newProjectName, newProjectModule;
    int frames = 0;
    float audioVolume = 1;
    Tool tool;
    player::PackageCommand package;   // --package
    std::pair<int, int> size{0, 0};
    double fpsOverride = -1;   // --fps
    bool lowLatency = false;   // --low-latency
    int vsyncOverride = -1;   // --vsync on|off
    std::string renderer;   // --renderer
    std::filesystem::path gameModuleFile;   // --game-module
    std::filesystem::path graphicsFile, graphicsSchema, materialSchema;
    bool graphicsInfo = false, noUserGraphics = false;
    bool preferencesInfo = false, noUserPreferences = false;   // --preferences, --no-user-preferences
    std::filesystem::path preferencesSchema;   // --preferences-schema
    bool audioVolumeSet = false;
    bool noPipelineCache = false, clearPipelineCache = false;
    bool noPipelineWarmup = false;   // --no-pipeline-warmup
    std::string graphicsPresetArg;   // --graphics-preset low|medium|high|ultra|fallback
    float renderScaleOverride = -1;
    std::vector<std::pair<std::string, std::string>> graphicsSets;   // --graphics-set key=value, --anti-aliasing
    bool listGpus = false;   // --gpus
    bool networkInfo = false;   // --network-info
    std::filesystem::path networkSchema;   // --network-schema
    bool releaseInfo = false;   // --release-info
    bool releaseTier = false;   // --release-tier
    std::filesystem::path cookInfoFile, cookVerify;   // --cook-info file [--verify folder]
    player::UserPathsCommand userPathsCommand;   // --user-dir, --profile, --user-paths, --import-legacy-saves
    std::filesystem::path releaseSchema;   // --release-schema
    std::filesystem::path levelSchema;   // --level-schema
    int steamStatus = 0;   // --steam-status: 1 client, 2 dedicated server
    player::NetTestCommand netTest;   // --net-test
    player::DiagnosticsCommand diagnostics;   // --diagnostics, --symbolize, --safe-mode, ...
    player::InstallCommand install;   // --verify-install (S13)
    std::filesystem::path netDump, netEncode;   // --net-dump, --net-encode
    std::filesystem::path imageStats, imageReference, imageCandidate;   // --image-stats, --image-diff
    std::filesystem::path materialInfo, textureInfo;   // --material-info, --texture-info
    std::filesystem::path effectInfo;   // --effect-info
    std::filesystem::path saveInfoFile, saveMigrateFile;   // --save-info, --save-migrate
    std::filesystem::path postProcessSchema, postProcessInfo, lutNeutral;   // A05/A06 tools, no project
    uint32_t lutNeutralSize = 0;
    std::filesystem::path terrainInfo, terrainImport;   // --terrain-info, --terrain-import (W1a)
    std::filesystem::path foliageInfo;   // --foliage-info (W2a)
    float terrainSpacing = 1.0f;   // --spacing
    float terrainLow = 0.0f, terrainHigh = 100.0f;   // --height-range
    std::filesystem::path lightingSchema, lightingExport, lightingImport;   // need no project
    double imageTolerance = 1.0;   // --tolerance
    std::array<double, 4> imageRegion{0, 0, 1, 1};   // --region
    std::filesystem::path output;   // --output: --package, --net-encode and the network tools
    bool devDirectJoin = false;   // --dev-direct-join
    bool joinSteam = false, joinLobby = false;
};

// The words after a flag. A row's values are checked before it's called: next() always has
// one to give for each. Rows with an optional value look with more() and peek() first.
class FlagArgs {
public:
    FlagArgs(const std::string& word, int& index, int count, char** words)
        : arg(word), m_i(index), m_argc(count), m_argv(words) {}
    const std::string& arg;   // the flag as written (rows with several names tell them apart)
    const char* next() { return m_argv[++m_i]; }
    const char* peek() const { return m_argv[m_i + 1]; }
    bool more() const { return m_i + 1 < m_argc; }

private:
    int& m_i;
    int m_argc;
    char** m_argv;
};

struct Flag {
    std::vector<std::string_view> names;
    int values = 0;   // words it needs after it; with fewer left it isn't this flag
    // Applies the flag; a value ends the program with that exit code (--help, --renderers).
    std::optional<int> (*apply)(ParseState&, FlagArgs&) = nullptr;
};

// Every flag, in groups (PlayerFlags*.cpp).
const std::vector<Flag>& playerFlags();
// The row named word that has `left` or fewer values to take, or null.
const Flag* findFlag(std::string_view word, int left);

void addProjectFlags(std::vector<Flag>& flags);
void addGraphicsFlags(std::vector<Flag>& flags);
void addReleaseFlags(std::vector<Flag>& flags);
void addNetworkFlags(std::vector<Flag>& flags);
void printHelp();

using ApplyFlag = std::optional<int> (*)(ParseState&, FlagArgs&);
inline void add(std::vector<Flag>& flags, std::vector<std::string_view> names, int values, ApplyFlag apply) {
    flags.push_back({std::move(names), values, apply});
}

// A whole number up to max, or a UsageError naming the flag.
uint64_t wholeNumber(const std::string& value, uint64_t max, const char* what);
// Lobby and Steam IDs: decimal, nonzero, 64-bit.
uint64_t steamNumber(const std::string& value, const char* what);

}   // namespace player
