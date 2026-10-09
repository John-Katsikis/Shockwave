#pragma once

#include <shockwave/Assets.h>
#include <shockwave/World.h>

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
// textures are saved by their name in Assets. Entity fields (a Parent, say) are saved as
// the position of the entity's block in the file, counting from 0, with -1 for none.
// "entity" and "end" start at the left edge; component names and fields are indented.
// Unknown components and fields are skipped on load, so newer files still open in older
// builds.
struct SceneFileHooks {
    // Extra top-level lines to write after the version line (an editor camera, say).
    std::function<void(std::ostream&)> writeHeader;
    // Called for top-level keys the loader doesn't know, with the rest of the line.
    std::function<void(const std::string& key, std::istream& rest)> readHeader;
};

bool saveScene(const World& world, const Assets& assets, const std::string& path, std::string& error,
               const SceneFileHooks& hooks = {});

// Replaces world with the file's contents on success. warnings collects things that
// loaded with a problem (an unknown mesh name, say).
bool loadScene(World& world, const Assets& assets, const std::string& path, std::string& error,
               const SceneFileHooks& hooks = {}, std::vector<std::string>* warnings = nullptr);

} // namespace shockwave
