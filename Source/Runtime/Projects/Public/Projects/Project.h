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
//     MyGame.swproject      name, game module, startup scene, runtime settings
//     Content/              images and models; scenes name them relative to here
//     Scenes/               .swscene files
//     Source/               the game module's C++ (optional)
//     Saved/                per-user editor state, autosaves, screenshots; not versioned
struct Project {
    std::filesystem::path file, root, startupScene, assetFolder, sceneFolder, theme;
    std::string name, gameModule;
    double tickRate = 60;
    int width = 1280, height = 720;
    bool vsync = true;
    double frameLimit = 0;   // frames per second, 0 = no cap beyond vsync
    bool lowLatency = false;   // wait for each frame to reach the screen before the next (Engine::setLowLatency)
    int shadowQuality = 3;   // 0 off, 1 low, 2 medium, 3 high, 4 ultra (Renderer's ShadowQuality)
    std::string renderer = "vulkan";   // the renderer plugin the player starts with
    bool audioHrtf = true;   // spatial audio for headphones (HRTF); false pans for speakers
    bool audioHrtfHigh = true;   // HRTF quality: high (bilinear) or low (nearest)

    std::filesystem::path statePath() const { return root / "Saved"; }
    // Project-relative with forward slashes ("Scenes/main.swscene"); paths outside the
    // project come back absolute.
    std::string relative(const std::filesystem::path& path) const;
};
// "off", "low", "medium", "high", "ultra" for 0..4; parse returns -1 for anything else.
const char* shadowQualityName(int quality);
int parseShadowQuality(const std::string& name);

// Strict versioned manifest. Failure leaves the destination unchanged.
bool loadProject(const std::filesystem::path& path, Project& project, std::string& error);
// Writes the manifest (through a temporary file, so a crash can't leave half of one).
bool saveProject(const Project& project, std::string& error);

// The manifest a path belongs to: the path itself if it's a .swproject, the one in it
// if it's a folder (the first by name if there are several), or else the nearest one in a folder
// above, so a scene file finds its project.
std::optional<std::filesystem::path> findProjectFile(const std::filesystem::path& path);

// Lays out a new project in folder (created if needed): <Name>.swproject (the name
// without spaces or punctuation), Content/, Scenes/ and a .gitignore for Saved/. The
// startup scene is Scenes/main.swscene;
// writing it is up to the caller, since loadProject wants it to exist. Refuses a folder
// that already holds a project.
bool createProject(
    const std::filesystem::path& folder, const std::string& name, const std::string& gameModule, Project& project,
    std::string& error);
}   // namespace shockwave
