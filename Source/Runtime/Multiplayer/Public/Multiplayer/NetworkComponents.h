#pragma once

// The components that make an entity part of a multiplayer session.
//
//   NetworkObject
//     key "door-west"                    # unique stable key of an authored root
//     prefab "Prefabs/Net Door.swprefab" # optional; how the manifest spawns copies
//     profile "game.door.v1"             # a registered replication profile
//     mode Interpolated                  # Static, Interpolated or PredictedCharacter
//     always_relevant 1                  # v1 replicates every object to every peer
//
//   NetworkSpawnPoint
//     team 0                             # 0..31; Arena uses 0 (blue) and 1 (orange)
//     order 0                            # picks spawn points in a fixed order
//
// The key is written once and never derived from names, file positions or hierarchy:
// renaming an entity keeps it. The editor gives a new NetworkObject a random key, and a
// fresh one to each duplicate; descriptive keys typed by hand are fine as long as they're
// unique. A prefab may leave its root's key empty; every copy in a scene sets its own.
// In v1 a network object is an unparented root with unit scale (its children are
// presentation), and prefab parts can't be network objects.

#include <Engine/World.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace shockwave::net {

struct NetworkObject {
    enum class Mode : int { Static, Interpolated, PredictedCharacter };
    std::string key;
    std::string prefab;   // inside Content, like Prefab paths
    std::string profile;
    Mode mode = Mode::Interpolated;
    bool alwaysRelevant = true;
};

struct NetworkSpawnPoint {
    int team = 0;
    int order = 0;
};

// Runtime only, never saved: the identity the host gives a network object in a session.
// IDs count up from 1 and are never reused within a session; slot 1 is the host, 2..64
// remote players, 0 nobody. Incarnation goes up on respawn and teleport.
struct NetworkIdentity {
    uint64_t id = 0;
    uint16_t ownerSlot = 0;
    uint32_t incarnation = 0;
};

// Registers NetworkObject and NetworkSpawnPoint (Inspector, scene files, --schema).
void registerNetworkComponents();

// Keys are 1..96 bytes of letters, digits and . _ : -
bool validNetworkKey(std::string_view key);
// A random key in UUID form ("3f2b8c1e-..."), from the operating system's secure random
// source. Empty if that source fails.
std::string newNetworkKey();
// Gives every NetworkObject in these entities' subtrees a fresh key: what the editor does
// to duplicates. Returns how many changed.
int renewNetworkKeys(World& world, std::span<const Entity> roots);
// Gives the entity's NetworkObject a key if it has none. True if it changed.
bool assignMissingNetworkKey(World& world, Entity entity);

}   // namespace shockwave::net
