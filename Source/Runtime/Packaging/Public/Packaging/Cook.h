#pragma once

// The cook manifest (S09, docs/Cooking.md): what a release of a game is made of, written
// by `player --cook` and read by `--cook-info` and release packaging.
//
//   Saved/Cook/linux-x86_64/Cook.swcook
//     swcook 1
//     application "Reactor Run"
//     game_module shippingdemo
//     target linux-x86_64
//     ...
//     level menu "Scenes/menu.swscene" 4f1c...
//     asset "Content/UI/badge.png" "texture" 2048 9a0e... 9a0e... menu training
//     root "Content/Localization/fr.swstrings" "localization catalog"
//
// Plain text, one record a line, sorted, so two cooks of the same content are the same file
// byte for byte and a diff between two says what changed. Nothing in it depends on when
// or where it was made: no times, no absolute paths. The content ID is a hash of the
// content records; the build ID adds the engine, its configuration, the target and the
// plugin ABI. Native programs are not claimed to be bit-reproducible, so their bytes stay
// out of both IDs.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

inline constexpr int kCookManifestVersion = 1;
// A manifest larger than this isn't read (a damaged or hostile file, not a game).
inline constexpr uintmax_t kMaxCookManifestBytes = 64ull * 1024 * 1024;

struct CookLevel {
    std::string id;   // the catalog's level id
    std::string scene;   // its scene, project-relative ("Scenes/menu.swscene")
    uint64_t closure = 0;   // the scene's closure hash: changes when anything it reaches does
    bool operator==(const CookLevel&) const = default;
};

// A file the game needs that no level reaches: catalogs, fonts, settings, the shell's
// screens, what Config/Package.swpackage's include patterns name for code to load.
struct CookRoot {
    std::string id;
    std::string why;   // "localization catalog"
    bool operator==(const CookRoot&) const = default;
};

struct CookAsset {
    std::string id;   // project-relative, forward slashes
    std::string kind;   // assetKindName: "texture"
    uint64_t bytes = 0;
    uint64_t hash = 0;   // of its bytes
    uint64_t closure = 0;   // its closure hash
    std::vector<std::string> levels;   // the levels that reach it, sorted; empty: a root's
    bool operator==(const CookAsset&) const = default;
};

struct CookManifest {
    int version = kCookManifestVersion;
    std::string application;   // the game's name
    std::string gameModule;   // "shippingdemo"; empty for a game with no code
    std::string target;   // "linux-x86_64"
    std::string buildConfig;   // the engine's CMake configuration
    std::string engineVersion;
    uint32_t pluginAbi = 0;
    bool release = false;   // made with --release: passed the release policy
    std::string entryLevel;
    std::vector<CookLevel> levels;   // by id
    std::vector<CookRoot> roots;   // by id
    std::vector<CookAsset> assets;   // by id: every level's closure and every root
    std::vector<std::string> licenses;   // the engine's license files, by name
    uint64_t contentId = 0, buildId = 0;   // computeIds()

    const CookAsset* asset(std::string_view id) const;
    // Sorts the lists and fills contentId and buildId from the rest.
    void computeIds();
    bool operator==(const CookManifest&) const = default;
};

std::string cookManifestText(const CookManifest& manifest);
// Reads Cook.swcook text. False with "line N: why" when it doesn't parse, or its IDs don't
// match its records (an edited or damaged manifest).
bool parseCookManifest(std::string_view text, CookManifest& manifest, std::string& error);
bool loadCookManifest(const std::filesystem::path& file, CookManifest& manifest, std::string& error);
bool saveCookManifest(const std::filesystem::path& file, const CookManifest& manifest, std::string& error);
std::string cookManifestJson(const CookManifest& manifest);
std::string hex64(uint64_t value);   // 16 lowercase hex digits

// Where a project's cook for a target goes: <project>/Saved/Cook/<target>/Cook.swcook.
std::filesystem::path cookManifestPath(const std::filesystem::path& projectRoot, std::string_view target);

// Checks every asset in the manifest against the files under root (a project, or a
// package's Game folder): each must be there with the bytes and hash it lists. Returns one
// line per problem: "Content/a.png: missing", "...: changed since the cook".
std::vector<std::string> verifyCookedFiles(const CookManifest& manifest, const std::filesystem::path& root);

// What a project's cook says, for the editor's Package window: the manifest at
// cookManifestPath(), each level's share of it, and (with verify) the files changed since.
struct CookReport {
    std::filesystem::path file;
    bool cooked = false;   // a manifest is there and reads
    std::string error;   // why it doesn't read, when there is one
    CookManifest manifest;
    struct Level {
        std::string id, scene;
        size_t assets = 0;   // in its closure, the scene included
        uint64_t bytes = 0;
        size_t shared = 0;   // of those, reached by other levels too
    };
    std::vector<Level> levels;   // in the manifest's order
    size_t files = 0;   // assets that are files (not built in)
    uint64_t bytes = 0;   // their total size
    bool verified = false;   // verify ran
    std::vector<std::string> changed;   // verifyCookedFiles against the project
};
CookReport cookReport(const std::filesystem::path& projectRoot, std::string_view target, bool verify);

// The targets this engine can cook for: only the one it was built for, since a cook
// records the engine and its modules. "linux-x86_64".
std::vector<std::string> cookTargets();

}   // namespace shockwave
