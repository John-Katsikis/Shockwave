#pragma once

// Components that tie a level's scene into the level catalog (S02, docs/Levels.md):
// where players arrive, where they leave for another level, checkpoints, and stable
// identities for things a save or another level refers to. They only describe; travel
// itself comes later (GameFlow, S05). Projects/LevelCatalog.h checks them.

#include <Engine/World.h>
#include <Projects/LevelCatalog.h>

#include <string>

namespace shockwave {

// Where a player arrives in this level: its Transform. `id` is what the catalog's
// default_spawn, a LevelExit's spawn and a Checkpoint's spawn name.
struct LevelSpawn {
    std::string id = "start";
    int team = -1;   // a game's own team filter; -1 for anyone
};

// A way out to another level. Only describes it: what triggers it (a physics trigger, a
// circuit) is the game's, so nothing teleports anyone just because it exists.
struct LevelExit {
    std::string destination;   // a catalog level id
    std::string spawn;   // a LevelSpawn there; empty for the destination's default
    bool enabled = true;
    bool oneShot = true;   // used once per visit
};

// A place to restart from, at one of this level's spawns. Offline only unless the game
// handles authority itself.
struct Checkpoint {
    std::string id;
    std::string spawn;   // a LevelSpawn in this scene
    bool enabled = true;
};

// A stable identity for an entity, for saves and references from other levels: a
// lowercase UUID, unique in the level. Names stay labels, never identity. Inside a prefab
// copy, an entity's identity combines with the copy's own (the copy's root needs one).
struct PersistentIdentity {
    std::string id;
};

// A new random identity for PersistentIdentity ("0f8fad5b-d9cb-469f-a165-70867728950e").
std::string newPersistentId();

// What a loaded scene says about its level, for validateLevelLinks.
LevelSceneFacts levelSceneFacts(const World& world);

}   // namespace shockwave
