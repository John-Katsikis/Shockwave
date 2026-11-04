#pragma once

// The level catalog (S02 of docs/ShippingImplementation.md, docs/Levels.md): which scenes
// are the game's levels, which one it starts in, and where each one leads. It lives in
// the file the project's `levels` key names (Config/Levels.swlevels by convention):
//
//   shockwave_levels 1
//   entry "menu"
//   level "menu"
//     scene "Scenes/menu.swscene"
//     kind menu
//   end
//   level "training"
//     scene "Scenes/training.swscene"
//     kind gameplay
//     default_spawn "start"
//     next "factory"
//     preload "Content/UI/loading.rml"
//   end
//
// A project without one still has a level: its startup scene, as the single implicit
// level "startup". Nothing writes a catalog just because a project was opened.
//
// Projects knows nothing about worlds, so the checks that need a scene's contents take
// LevelSceneFacts, which the caller (Player, the editor) reads from the loaded scenes.

#include <Projects/Diagnostic.h>

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

struct Project;

enum class LevelKind { Menu, Gameplay, Ending };
const char* levelKindName(LevelKind kind);   // "menu", "gameplay", "ending"

struct LevelEntry {
    std::string id;   // [a-z][a-z0-9_-]{0,63}, unique in the catalog
    std::string scene;   // project-relative, forward slashes: "Scenes/factory.swscene"
    LevelKind kind = LevelKind::Gameplay;
    std::string defaultSpawn;   // a LevelSpawn id in the scene; empty: the scene's own start
    std::string next;   // the default destination (another level id); never automatic
    std::vector<std::string> preload;   // project-relative files loaded before the level
    int line = 0;   // where `level` is, for diagnostics; 0 when not from a file
    std::map<std::string, int> lines;   // where each key was ("next" -> 14), for diagnostics

    bool operator==(const LevelEntry& other) const {
        return id == other.id && scene == other.scene && kind == other.kind && defaultSpawn == other.defaultSpawn &&
               next == other.next && preload == other.preload;
    }
};

struct LevelCatalog {
    std::string entry;   // the level the game starts in
    std::vector<LevelEntry> levels;   // in file order
    // True for a project without a catalog file: one level, "startup", its startup scene.
    bool implicit = false;
    int entryLine = 0;

    const LevelEntry* find(std::string_view id) const;
    LevelEntry* find(std::string_view id);
    bool operator==(const LevelCatalog& other) const {
        return entry == other.entry && levels == other.levels && implicit == other.implicit;
    }
};

inline constexpr const char* kImplicitLevelId = "startup";

// Level and spawn ids: [a-z][a-z0-9_-]{0,63}.
bool validLevelId(std::string_view id);
// A PersistentIdentity id: a UUID in lowercase hex, "0f8fad5b-d9cb-469f-a165-70867728950e".
bool validPersistentId(std::string_view id);

// ---- The file -----------------------------------------------------------------------

// Reads catalog text. `shown` names the file in diagnostics. Returns false, with at least
// one error, when the file can't be used; on success `out` is replaced. Every problem the
// text has on its own is reported (unknown and duplicate keys, bad ids, kinds, paths that
// leave the project, duplicate levels, a missing or unknown entry, a `next` that names no
// level); checks against the disk come from validateLevelCatalog.
bool parseLevelCatalog(
    std::string_view text, const std::string& shown, LevelCatalog& out, std::vector<Diagnostic>& diagnostics);
bool readLevelCatalog(
    const std::filesystem::path& file, const std::string& shown, LevelCatalog& out,
    std::vector<Diagnostic>& diagnostics);
// The text parseLevelCatalog reads back exactly.
std::string levelCatalogText(const LevelCatalog& catalog);
// Through a temporary file, so a crash can't leave half a catalog.
bool writeLevelCatalog(const std::filesystem::path& file, const LevelCatalog& catalog, std::string& error);

// The catalog file the project names (`levels` key), or empty when it has none.
std::filesystem::path levelCatalogPath(const Project& project);
// The project's levels: its catalog file, or the implicit "startup" level without one.
// False (with diagnostics) when the file doesn't read.
bool projectLevels(const Project& project, LevelCatalog& out, std::vector<Diagnostic>& diagnostics);
LevelCatalog implicitLevelCatalog(const Project& project);

// The catalog against the project's files: each scene exists, is a .swscene inside the
// project (no `..` or symlink leaving it), and isn't spelt like another only by case;
// each preload exists inside the project. Levels nothing reaches from the entry (through
// `next` and exits, when facts are given) are warnings.
void validateLevelCatalog(
    const Project& project, const LevelCatalog& catalog, const std::string& shown,
    std::vector<Diagnostic>& diagnostics);

// ---- What's in each level's scene ----------------------------------------------------

// Read from a loaded scene by the caller (Engine's levelSceneFacts).
struct LevelSceneFacts {
    struct Spawn {
        std::string id, entity;
    };
    struct Exit {
        std::string destination, spawn, entity;
        bool enabled = true;
    };
    struct Checkpoint {
        std::string id, spawn, entity;
    };
    struct Identity {
        std::string id;   // the effective identity: "<instance>/<local>" inside a prefab copy
        std::string entity;
        std::string problem;   // why it isn't valid, or empty
    };
    bool loaded = false;
    std::string loadError;   // the scene (or a prefab in it) didn't load
    std::vector<Spawn> spawns;
    std::vector<Exit> exits;
    std::vector<Checkpoint> checkpoints;
    std::vector<Identity> identities;
    int primaryCameras = 0;
    bool hasUserInterface = false;   // a UIDocument: a menu can be drawn by UI alone
};

// The links between levels: default spawns exist, spawn and checkpoint ids are unique,
// exits lead to catalog levels and spawns that exist there, checkpoints name a local
// spawn, persistent ids are valid and unique, gameplay levels have a camera (menus and
// endings may be UI only), and unreachable levels (from the entry, along `next` and
// exits) are warnings. `facts` is keyed by level id; a level with no facts isn't checked.
void validateLevelLinks(
    const LevelCatalog& catalog, const std::map<std::string, LevelSceneFacts>& facts, const std::string& shown,
    std::vector<Diagnostic>& diagnostics);

// Levels reachable from the entry along `next` and (when given) enabled exits.
std::vector<std::string>
reachableLevels(const LevelCatalog& catalog, const std::map<std::string, LevelSceneFacts>* facts = nullptr);

// docs/LevelSchema.md: the format, the components and every diagnostic code.
std::string levelSchema();

}   // namespace shockwave
