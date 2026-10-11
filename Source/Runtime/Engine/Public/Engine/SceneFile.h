#pragma once

#include <Assets/Assets.h>
#include <Engine/World.h>

#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace shockwave {

// Scenes are plain text (.swscene) so they diff and merge cleanly in version control:
//
//   shockwave_scene 2
//
//   entity "Crate"
//     Transform
//       position 0 0.5 0
//       rotation 0 20 0
//       scale 1 1 1
//     MeshRenderer
//       mesh cube
//       ...
//   end
//
// Every registered component (see Components.h) is saved with its fields. Meshes and
// textures are saved by their name in Assets, enums by their option name. Entity fields
// (a Parent, say) are saved as the target's name when it's unique, otherwise as the
// position of its block in the file counting from 0; "none" when empty. Both load.
// "entity" and "end" start at the left edge; component names and fields are indented.
// Unknown components and fields are skipped on load with a warning, so newer files
// still open in older builds.
//
// Entities with a Prefab component are copies of a prefab (Prefab.h): only what differs
// from the prefab is written, and loading makes the copy again.
//
// The format is meant to be written by people, tools and AI alike: writeSceneSchema
// documents every component, and `player --check` validates files.
struct SceneFileHooks {
    // Extra top-level lines to write after the version line (an editor camera, say).
    std::function<void(std::ostream&)> writeHeader;
    // Called for top-level keys the loader doesn't know, with the rest of the line.
    std::function<void(const std::string& key, std::istream& rest)> readHeader;
    // Load into these entities instead of creating new ones: the nth block fills the nth
    // live entity. A scene written from a world with the same entities comes back with
    // the same handles (World::withoutComponents gives such a world).
    const World* entities = nullptr;
    // Write the prefab header (shockwave_prefab) instead of the scene one. See Prefab.h.
    bool prefab = false;
    // Write every entity in full, prefab parts included, and read them back without
    // remaking prefab copies: an exact round trip in memory (game code hot reload).
    bool exact = false;
};

bool saveScene(
    const World& world, const Assets& assets, const std::string& path, std::string& error,
    const SceneFileHooks& hooks = {});

// Replaces world with the file's contents on success. warnings collects things that
// loaded with a problem (an unknown mesh name, say).
bool loadScene(
    World& world, const Assets& assets, const std::string& path, std::string& error, const SceneFileHooks& hooks = {},
    std::vector<std::string>* warnings = nullptr);

// The same, to and from a stream: a scene held in memory (the editor keeps its undo
// history as scene text while game code reloads). `path` only names the source in errors.
void writeScene(std::ostream& out, const World& world, const Assets& assets, const SceneFileHooks& hooks = {});
bool readScene(
    World& world, const Assets& assets, std::istream& in, const std::string& path, std::string& error,
    const SceneFileHooks& hooks = {}, std::vector<std::string>* warnings = nullptr);

// Writes a Markdown reference for authoring scenes: the file layout, every registered
// component with its fields, value syntax and defaults, and the asset names available.
// Main thread, after the game's types and assets are registered.
void writeSceneSchema(std::ostream& out, const Assets& assets);

}   // namespace shockwave
