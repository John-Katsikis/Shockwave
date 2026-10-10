#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace shockwave {
// A game project: a folder holding a .swproject manifest (see docs/Projects.md),
// its assets and scenes. Paths resolve against the manifest, never the process
// working directory.
//
//   MyGame/
//     shockwave.swproject   name, game module, startup scene, runtime settings
//     assets/               images and models; scenes name them relative to here
//     scenes/               .swscene files
//     .shockwave/           per-user editor state (camera, autosaves); not versioned
struct Project {
    std::filesystem::path file, root, startupScene, assetFolder, sceneFolder, theme;
    std::string name, gameModule;
    double tickRate = 60;
    int width = 1280, height = 720;
    bool vsync = true;

    std::filesystem::path statePath() const { return root / ".shockwave"; }
    // Project-relative with forward slashes ("scenes/main.swscene"); paths outside the
    // project come back absolute.
    std::string relative(const std::filesystem::path& path) const;
};
// Strict versioned manifest. Failure leaves the destination unchanged.
bool loadProject(const std::filesystem::path& path, Project& project, std::string& error);
// Writes the manifest (through a temporary file, so a crash can't leave half of one).
bool saveProject(const Project& project, std::string& error);

// The manifest a path belongs to: the path itself if it's a .swproject, the one in it
// if it's a folder (shockwave.swproject first), or else the nearest one in a folder
// above, so a scene file finds its project.
std::optional<std::filesystem::path> findProjectFile(const std::filesystem::path& path);

// Lays out a new project in folder (created if needed): shockwave.swproject, assets/,
// scenes/ and a .gitignore for .shockwave/. The startup scene is scenes/main.swscene;
// writing it is up to the caller, since loadProject wants it to exist. Refuses a folder
// that already holds a project.
bool createProject(const std::filesystem::path& folder, const std::string& name, const std::string& gameModule,
                   Project& project, std::string& error);
} // namespace shockwave
