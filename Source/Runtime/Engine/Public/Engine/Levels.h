#pragma once

// Components that tie a level's scene into the level catalog (S02, docs/Levels.md):
// where players arrive, where they leave for another level, checkpoints, and stable
// identities for things a save or another level refers to. They only describe; travel
// is GameFlow's (GameFramework/GameFlow.h). Projects/LevelCatalog.h checks them.

#include <Engine/Components.h>
#include <Engine/World.h>
#include <Projects/LevelCatalog.h>

#include <string>
#include <string_view>

namespace shockwave {

// Where a player arrives in this level: its Transform. `id` is what the catalog's
// default_spawn, a LevelExit's spawn and a Checkpoint's spawn name.
struct LevelSpawn {
    std::string id = "start";
    int team = -1;   // a game's own team filter; -1 for anyone
};

// A way out to another level. Only describes it: what triggers it (a physics trigger, a
// circuit) is the game's, so nothing teleports anyone just because it exists. The game
// leaves through it with game.flow().useExit(entity) or the Level.UseExit node.
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

// Where a level was entered (S05, game.arrival): the LevelSpawn asked for, and where it
// is in the world. found is false when no spawn was asked for and the scene has no
// "start" spawn, or the spawn has no Transform; the game places players itself then.
struct LevelArrival {
    std::string spawn;   // the id asked for (or the catalog's default), else "start" if there is one
    Entity entity = kNullEntity;
    bool found = false;
    Transform transform;   // the spawn's, in world space
};
// The LevelSpawn with this id, or kNullEntity. Several with one id: the first created.
Entity findLevelSpawn(const World& world, std::string_view id);
// Where `spawn` is; empty looks for "start".
LevelArrival levelArrival(const World& world, const std::string& spawn);

}   // namespace shockwave
