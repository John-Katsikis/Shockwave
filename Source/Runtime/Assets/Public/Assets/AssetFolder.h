#pragma once

#include <Assets/Assets.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

class JobSystem;
class Renderer;

// Keeps a folder of source files (assets/ in a project) imported into Assets. Each file
// is named by its path inside the folder with forward slashes ("textures/bricks.png"),
// which is how scene files refer to it. Saving a file in any program re-imports it, and
// everything using it updates on the next frame.
//
// Imports images and Assimp models with per-part materials and node transforms.
// Imports decode in parallel on the job system.
//
// Main thread only.
class AssetFolder {
public:
    enum class Kind { Texture, Model, Other };
    struct Dependency {
        std::filesystem::path path;
        bool exists = false;
        uintmax_t bytes = 0;
        std::filesystem::file_time_type modified{};
    };

    struct File {
        std::string name;              // path inside the folder, forward slashes
        std::filesystem::path path;    // on disk
        Kind kind = Kind::Other;
        uintmax_t bytes = 0;
        std::filesystem::file_time_type modified{};
        bool imported = false;
        std::string error;             // why the last import failed, if it did
        TextureHandle texture;         // Kind::Texture, once imported
        uint32_t width = 0, height = 0;
        ModelHandle model;
        std::vector<Dependency> dependencies;
        std::vector<std::string> warnings;
    };

    struct Change {
        enum class What { Added, Reloaded, Failed, Removed };
        What what;
        std::string name;
        std::string error;
    };

    AssetFolder(Renderer& renderer, Assets& assets, JobSystem& jobs);

    // Switches to a folder and imports everything in it. A missing folder is fine: it
    // just has no files until it's created.
    std::vector<Change> open(std::filesystem::path root);
    // Imports new and changed files and notices removed ones.
    std::vector<Change> refresh(bool force = false);
    // refresh(), at most every interval seconds; pass the frame time.
    std::vector<Change> poll(double frameDt, double interval = 0.5);

    const std::filesystem::path& root() const { return m_root; }
    const std::vector<File>& files() const { return m_files; }   // sorted by name
    const File* find(std::string_view name) const;

private:
    Renderer& m_renderer;
    Assets& m_assets;
    JobSystem& m_jobs;
    std::filesystem::path m_root;
    std::vector<File> m_files;
    double m_pollTimer = 0.0;
};

} // namespace shockwave
