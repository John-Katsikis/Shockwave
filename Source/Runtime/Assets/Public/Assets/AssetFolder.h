#pragma once

#include <Assets/Assets.h>
#include <Assets/ImportSettings.h>
#include <Assets/MeshOptimise.h>
#include <Core/Jobs.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

class JobSystem;
class Renderer;
class TextureCompressor;

// Keeps a folder of source files (Content/ in a project) imported into Assets. Each file
// is named by its path inside the folder with forward slashes ("textures/bricks.png"),
// which is how scene files refer to it. Saving a file in any program re-imports it, and
// everything using it updates on the next frame.
//
// Imports images, .ktx2 files and Assimp models with per-part materials and node
// transforms. Imports decode in parallel on the job system. Meshes are optimised and given
// levels of detail, and textures compressed, as docs/Optimisation.md describes.
//
// Main thread only.
class AssetFolder {
public:
    enum class Kind { Texture, Model, Audio, Other };
    struct Dependency {
        std::filesystem::path path;
        bool exists = false;
        uintmax_t bytes = 0;
        std::filesystem::file_time_type modified{};
    };

    struct File {
        std::string name;   // path inside the folder, forward slashes
        std::filesystem::path path;   // on disk
        Kind kind = Kind::Other;
        uintmax_t bytes = 0;
        std::filesystem::file_time_type modified{};
        bool imported = false;
        std::string error;   // why the last import failed, if it did
        TextureHandle texture;   // Kind::Texture, once imported
        uint32_t width = 0, height = 0;
        ModelHandle model;
        std::vector<Dependency> dependencies;
        std::vector<std::string> warnings;

        // ---- Optimisation (docs/Optimisation.md) ----
        ImportSettings settings;   // the folder's defaults with the .swimport file's overrides
        uint32_t textures = 0;   // on the GPU: 1 for a texture, a model's colour textures
        uint32_t compressedTextures = 0;   // of those, how many are compressed (BC7)
        uint32_t compressing = 0;   // still waiting for the background compressor
        uint64_t gpuBytes = 0;   // GPU memory those textures take, mipmaps included
        uint64_t uncompressedBytes = 0;   // what they would take as RGBA8
        uint64_t compressedFileBytes = 0;   // their .ktx2 files (in the cache, or the asset itself)
        MeshOptimiseReport meshes;   // models: reordering and levels of detail
    };

    struct Change {
        enum class What { Added, Reloaded, Failed, Removed, Compressed };
        What what;
        std::string name;
        std::string error;
    };

    AssetFolder(Renderer& renderer, Assets& assets, JobSystem& jobs);
    AssetFolder(Assets& assets, JobSystem& jobs);   // audio-only tools, no GPU publication
    ~AssetFolder();
    AssetFolder(const AssetFolder&) = delete;
    AssetFolder& operator=(const AssetFolder&) = delete;

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

    // ---- Optimisation (docs/Optimisation.md) ----
    // Import settings for every asset; a .swimport file beside one overrides them. A change
    // re-imports everything at the next refresh. Set before open() to import once.
    void setImportDefaults(const ImportSettings& defaults);
    const ImportSettings& importDefaults() const { return m_defaults; }
    // Where compressed textures are kept between runs (a project's Saved/Cache/Textures).
    // Without one, textures are compressed again every run.
    void setCacheFolder(std::filesystem::path folder) { m_cacheFolder = std::move(folder); }
    const std::filesystem::path& cacheFolder() const { return m_cacheFolder; }
    // Off: textures not in the cache stay uncompressed instead of being compressed in the
    // background. For short-lived tools (player --check) that would quit before it's done.
    void setBackgroundCompression(bool on) { m_backgroundCompression = on; }
    // Textures waiting for the background compressor. Each is drawn uncompressed until
    // it's done; refresh() swaps it in and reports What::Compressed.
    size_t compressing() const;
    // Compresses everything still waiting now, on every thread, and swaps it in. For tools
    // (player --optimise) and tests; the editor and games let it happen in the background.
    std::vector<Change> finishCompressing();

private:
    struct AudioImport;
    std::vector<std::shared_ptr<AudioImport>> m_audioImports;
    JobCounter m_audioJobs;
    bool m_initialImport = false;
    Renderer* m_renderer = nullptr;
    Assets& m_assets;
    JobSystem& m_jobs;
    std::filesystem::path m_root;
    std::vector<File> m_files;
    double m_pollTimer = 0.0;
    // Optimisation
    File* findFile(std::string_view name);
    void publishCompressed(std::vector<Change>& changes);
    ImportSettings m_defaults;
    bool m_reimportAll = false;
    bool m_backgroundCompression = true;
    std::filesystem::path m_cacheFolder;
    std::unique_ptr<TextureCompressor> m_compressor;
};

}   // namespace shockwave
