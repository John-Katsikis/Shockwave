#pragma once

// How long a runtime level lives, and how it ends (S03, docs/LevelLifetime.md).
//
// A PlaySession either plays an authored world and puts it back on Stop (the editor's and
// tools' original behaviour, RestoreAuthoredSnapshot), or plays a world that belongs to
// the runtime and is simply discarded (OwnedRuntimeWorld): LevelHost uses that for every
// level it travels to, and never copies a whole world to do it.

#include <cstdint>
#include <string>

namespace shockwave {

enum class SessionWorldPolicy {
    RestoreAuthoredSnapshot,   // start() copies the world, stop() puts the copy back
    OwnedRuntimeWorld,   // the world is the runtime's: stop() tears down and leaves it
};

enum class SessionSettingsPolicy {
    RestoreOnStop,   // graphics changed during Play are put back on stop()
    PreserveUserSettings,   // a player's choices outlive the level (travel)
};

// Why a level's session is starting, for GameMode::beginPlay (game.startReason).
enum class StartReason {
    Fresh,   // a new start: the entry level, a travel, Play in the editor
    RestoreSave,   // the world is about to be replaced by a save game
    OnlineBaseline,   // a client starting from the host's state
};
const char* startReasonName(StartReason reason);   // "fresh", "restore_save", "online_baseline"

// Where game code asks to go: a level ID from the level catalog (docs/Levels.md) and,
// optionally, the LevelSpawn to arrive at there.
struct LevelRequest {
    std::string level;
    std::string spawn = {};
    bool operator==(const LevelRequest&) const = default;
};

// Every accepted request ends exactly once, in one of these.
enum class TravelStatus { Succeeded, Failed, Cancelled };
const char* travelStatusName(TravelStatus status);   // "succeeded", "failed", "cancelled"

struct TravelResult {
    uint64_t id = 0;   // what LevelHost::request returned
    LevelRequest request;
    TravelStatus status = TravelStatus::Failed;
    std::string error;   // why it failed or was cancelled
    uint64_t epoch = 0;   // the level epoch it arrived in (Succeeded), else the one it left
};

// How one level start or change went, phase by phase (S04, docs/AssetResidency.md). The
// times are measured on the main thread; the loading screen's weights (S05) are only an
// estimate of them. The asset counts come from the level's dependency closure.
struct LoadReport {
    LevelRequest request;
    std::string from;   // the level it left; empty for the first
    uint64_t epoch = 0;   // the epoch it arrived in, or the one it stayed in
    TravelStatus status = TravelStatus::Succeeded;
    std::string error;
    // Milliseconds per phase: Validate (the request), Read (the scene into a separate
    // world), Prepare (its dependency closure and the budget), Retire (the old level and
    // its lease), Upload (the new world and lease), Start (the session and beginPlay).
    double validateMs = 0, readMs = 0, prepareMs = 0, retireMs = 0, uploadMs = 0, startMs = 0, totalMs = 0;
    size_t assets = 0;   // in the new level's closure
    size_t shared = 0, added = 0, retired = 0;
    uint64_t addedCpuBytes = 0, addedGpuBytes = 0, retiredCpuBytes = 0, retiredGpuBytes = 0;
    uint64_t residentCpuBytes = 0, residentGpuBytes = 0;   // afterwards
    uint64_t peakCpuBytes = 0, peakGpuBytes = 0;
};
const char* loadPhaseName(int phase);   // 0..5: "validate", "read", "prepare", "retire", "upload", "start"

}   // namespace shockwave
