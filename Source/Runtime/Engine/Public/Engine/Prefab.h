#pragma once

// Prefabs: reusable entity templates, like Unreal's Blueprint actors or Unity's prefabs.
//
// A prefab is a .swprefab file inside Content. It's the same text as a scene, with its own
// header line, and its first entity is the root that everything else hangs under:
//
//   shockwave_prefab 1
//
//   entity "Street lamp"
//     Transform
//     MeshRenderer
//       mesh cube
//   end
//
//   entity "Bulb"
//     Parent
//       entity "Street lamp"
//     Light
//       type Point
//   end
//
// A scene (or another prefab) uses one with an entity that has a Prefab component. Only
// what differs from the prefab is written, usually just where it stands:
//
//   entity "Lamp by the door"
//     Prefab
//       path "Prefabs/Street lamp.swprefab"
//     Transform
//       position 4 0 -2
//   end
//
// Loading makes the copy: the root gets the prefab root's components, with the scene's
// values on top, and the rest of the prefab is created under it as parts (PrefabPart).
// Parts aren't saved; change them in the prefab and every copy follows, hot reload
// included. Prefabs can use other prefabs, to any depth.
//
// docs/Prefabs.md is the guide; `player --check` validates every .swprefab in Content.

#include <Engine/World.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace shockwave {

class Assets;
struct PrefabAsset;
struct Transform;

// On the root of a prefab copy: which prefab it is. Saved as the "Prefab" component.
struct PrefabInstance {
    std::string path;   // inside Content, forward slashes: "Prefabs/Street lamp.swprefab"
    // Runtime, not saved: the loaded prefab the copy was made from. updatePrefabInstances
    // remakes the copy when this is out of date.
    std::shared_ptr<const PrefabAsset> source;
};

// Marks an entity that a prefab copy made under its root. Not saved, and not edited on
// its own: change the prefab, or unpack the copy to make the parts ordinary entities.
struct PrefabPart {};

// A prefab file, loaded. Shared and never changed once loaded; a newer version of the
// file replaces it in the library and marks this one stale.
struct PrefabAsset {
    std::string path;   // the library key: inside Content, forward slashes
    World world;   // the prefab's entities, with prefabs it uses already expanded
    Entity root;   // the first entity in the file
    std::vector<std::string> uses;   // other prefabs it contains, at any depth
    std::vector<std::string> warnings;   // "file:line: problem", from loading
    std::atomic<bool> stale{false};
};

// Loads and caches prefab files. One per process (prefabs()); thread-safe.
class PrefabLibrary {
public:
    // Where prefab paths are looked up, and the assets that names in them refer to.
    // Call it when a project opens; it forgets everything loaded before.
    void setup(std::filesystem::path contentFolder, const Assets* assets);
    std::filesystem::path contentFolder() const;
    const Assets* assets() const;
    // The file a prefab path names.
    std::filesystem::path file(std::string_view path) const;
    // "Prefabs/Lamp.swprefab" for a file inside Content, forward slashes.
    std::string pathOf(const std::filesystem::path& file) const;
    // How the library names a prefab path ("./Prefabs//a.swprefab" -> "Prefabs/a.swprefab").
    std::string key(std::string_view path) const;

    // The prefab, loaded on first use and cached after. Null if it can't be loaded, with
    // the reason in error.
    std::shared_ptr<const PrefabAsset> load(std::string_view path, std::string* error = nullptr);
    std::shared_ptr<const PrefabAsset> load(std::string_view path, const Assets& assets, std::string* error = nullptr);
    // Reads the file again, then every loaded prefab that uses it. Returns the paths that
    // were reloaded.
    std::vector<std::string> reload(std::string_view path);
    // Reloads prefabs whose files changed on disk, checking at most every interval
    // seconds; pass the frame time. Returns the paths that were reloaded.
    std::vector<std::string> poll(double frameDt, double interval = 0.5);
    void clear();

private:
    struct Entry {
        std::shared_ptr<const PrefabAsset> asset;   // null when loading failed
        std::string error;
        std::filesystem::file_time_type modified{};   // min() when the file was missing
        std::vector<std::string> uses;
    };
    std::shared_ptr<const PrefabAsset> loadLocked(const std::string& key, const Assets& assets, std::string* error);

    mutable std::recursive_mutex m_mutex;   // loading a prefab loads the ones it uses
    std::filesystem::path m_contentFolder;
    const Assets* m_assets = nullptr;
    std::unordered_map<std::string, Entry> m_entries;
    std::vector<std::string> m_loading;   // the chain being loaded, to catch a prefab using itself
    double m_pollTimer = 0.0;
};

PrefabLibrary& prefabs();

// ---- Using prefabs ----------------------------------------------------------------------
// These change the world's structure, so they follow World's rules: one thread, nothing
// iterating the world meanwhile. In game code that means GameMode hooks (beginPlay,
// fixedUpdate, frame), not inside systems.

// A new copy of the prefab. Without `at` the root keeps the prefab's own Transform. Returns
// the root, or kNullEntity (with a warning in the log) when the prefab can't be loaded.
Entity spawnPrefab(World& world, std::string_view path, const Transform* at = nullptr, Entity parent = kNullEntity);
Entity spawnPrefab(
    World& world, std::shared_ptr<const PrefabAsset> prefab, const Transform* at = nullptr,
    Entity parent = kNullEntity);

// Remakes copies whose prefab file was reloaded or whose Prefab path was changed,
// keeping what each copy changed on its root. Cheap when nothing changed; call it once a
// frame where prefabs can change. Returns how many copies were remade.
size_t updatePrefabInstances(World& world);

// The copy e belongs to: e itself if it's a copy's root, the root of the copy that made it
// if it's a part (the outermost, for parts of nested prefabs), otherwise kNullEntity.
Entity prefabRootOf(const World& world, Entity e);
// The nearest copy root above e (not e itself), or kNullEntity. For a part, the copy
// whose prefab file holds it.
Entity prefabOwnerOf(const World& world, Entity e);

// Turns a copy into ordinary entities: the root loses its Prefab, its parts become plain
// entities. Prefabs nested in it stay prefab copies.
void unpackPrefab(World& world, Entity root);

// Writes e and everything under it as a prefab file. e becomes the prefab root, placed at
// the origin. When e is a prefab copy, the new prefab builds on that one (a variant).
bool savePrefab(
    const World& world, Entity e, const Assets& assets, const std::filesystem::path& file, std::string& error);
// Writes a copy's changes back to its prefab: the root's components (but not where the
// copy stands, or its name) and entities added under it. Every copy then follows.
bool applyPrefabInstance(const World& world, Entity root, const Assets& assets, std::string& error);

}   // namespace shockwave
