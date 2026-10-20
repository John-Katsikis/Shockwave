#pragma once

// The offline network checks `player --check` and the editor share. None of them start
// Steam, open a socket or need a display; all report problems as text with file:line.

#include <Multiplayer/NetworkManifest.h>
#include <Multiplayer/NetworkSettings.h>
#include <Multiplayer/Replication.h>

#include <Engine/World.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {
struct Project;
}

namespace shockwave::net {

// A project's network files, as read.
struct NetworkProject {
    bool present = false;   // Config/Network.swnetwork exists; without it the game is offline only
    NetworkSettingsFile settings;
    std::string settingsShown;   // "Config/Network.swnetwork"
    std::optional<NetworkManifest> manifest;   // when the settings' manifest exists and parses
    std::string manifestShown;
};

// Reads Config/Network.swnetwork and the manifest it names. Problems in either go to
// problems; a missing settings file isn't one. False on a problem that stops checking.
bool loadNetworkProject(const Project& project, NetworkProject& out, std::vector<NetProblem>& problems);

// Settings rules (tick_rate equal to the project's), the manifest, the registry's
// profiles, and whether max_players' snapshots fit the bandwidth budget. The registry
// must be frozen.
void validateNetworkProject(
    const Project& project, const NetworkProject& network, const ReplicationRegistry& registry,
    std::vector<NetProblem>& problems);

// What one scene's (or prefab's) network objects need: unique valid keys, registered
// profiles and their components, unparented unit-scale roots, no network prefab parts,
// what PredictedCharacter requires, and distinct spawn points. Quiet for a world with
// no network components. `shown` names the scene; `prefab` relaxes the key rule (a prefab
// root may leave its key for copies to set).
void validateNetworkWorld(
    const World& world, const ReplicationRegistry& registry, std::string_view shown, bool prefab,
    std::vector<NetProblem>& problems);

// The collision rule for predicted characters (plan 9.2): online they move against static
// collision only, on the host and in their clients' prediction alike. So in a scene where
// one plays (it has a PredictedCharacter, or it's a network scene, with network objects or
// spawn points, and predictedSpawns: the manifest spawns one),
// a non-sensor kinematic body is an error (no moving platforms or doors to stand on or be
// pushed by in v1) and a non-sensor dynamic body a warning (characters pass through it).
// Quiet for a scene without predicted characters.
void validatePredictionWorld(
    const World& world, std::string_view shown, bool predictedSpawns, std::vector<NetProblem>& problems);
// Whether a manifest prefab's root is a PredictedCharacter.
bool predictedPrefab(const World& prefabWorld, Entity root);

// The bytes per second the host sends each peer for player state: max_players records of
// the largest registered per-player profile, at snapshot_rate, split into snapshot
// parts of max_snapshot_bytes. 0 when the game registers no per-player profile.
uint64_t estimatePlayerSnapshotBytesPerSecond(const NetworkSettings& settings, const ReplicationRegistry& registry);

// What starting a session needs, checked before any transport or socket exists: enabled
// settings without errors, a valid manifest naming the scene, a frozen registry. On
// failure every reason is in problems.
bool readyForSession(
    const Project& project, const NetworkProject& network, const ReplicationRegistry& registry, uint16_t sceneId,
    std::vector<NetProblem>& problems);

}   // namespace shockwave::net
