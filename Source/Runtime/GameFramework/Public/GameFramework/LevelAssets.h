#pragma once

// A project's asset graph and what a level costs to keep resident (S04,
// docs/AssetResidency.md). The player and the editor share these for travel leases,
// --asset-dependencies, --check and the Resource residency panel.

#include <Assets/AssetGraph.h>
#include <Assets/AssetResidency.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

class AssetFolder;
class Assets;
class Renderer;
struct Project;

// A graph over the project, with what the AssetFolder imported (a model's textures) and
// the names Assets has registered (built-in meshes and textures). Either may be null.
// The folder and assets must outlive the graph.
std::unique_ptr<AssetGraph>
makeProjectAssetGraph(const Project& project, const AssetFolder* folder, const Assets* assets);

// Every existing asset a root reaches (itself included), with costs: CPU is the file's
// size, GPU what the AssetFolder measured for its textures plus a model's vertex and index
// buffers. Built-ins cost nothing here: they belong to the application. Adds the root to
// the graph first.
std::vector<AssetCost>
assetCosts(AssetGraph& graph, std::string_view root, const AssetFolder* folder, const Assets* assets);

// The graph's id for a scene file on disk ("Scenes/a.swscene").
std::string sceneAssetId(const AssetGraph& graph, const std::filesystem::path& scene);

// The --asset-dependencies JSON for these roots (format 1): each root's closure with
// kinds, bytes and closure hashes, the edges between them, and the diagnostics.
// roots: (name, id) pairs, a level's id and its scene, say.
std::string assetDependenciesJson(
    AssetGraph& graph, const std::vector<std::pair<std::string, std::string>>& roots, const AssetFolder* folder,
    const Assets* assets);

// Loads and unloads a project's Content for AssetResidency (asset_loading levels,
// docs/AssetResidency.md): ids under the assets folder ("Content/textures/a.png") go to the
// AssetFolder, which should be on demand; anything else (a scene, a built-in) has nothing to
// import. The folder and renderer must outlive it; the renderer may be null.
class AssetFolderLoader final : public AssetResidency::Loader {
public:
    AssetFolderLoader(const Project& project, AssetFolder& folder, Renderer* renderer);
    // About how much decoded data a load holds at once (AssetFolder::LoadOptions).
    void setStagingBytes(uint64_t bytes) { m_stagingBytes = bytes; }
    bool load(
        const std::vector<std::string>& ids, const std::function<bool()>& cancelled, std::vector<std::string>& loaded,
        std::string& error) override;
    void unload(const std::vector<std::string>& ids) override;
    size_t pendingFrees() const override;
    // The last load's batches and the most decoded data one held, for load reports.
    size_t lastBatches() const { return m_lastBatches; }
    uint64_t lastPeakStagedBytes() const { return m_lastPeak; }

private:
    std::string name(const std::string& id) const;   // inside the folder; empty if not in it
    std::string m_prefix;   // "Content/"
    AssetFolder& m_folder;
    Renderer* m_renderer;
    uint64_t m_stagingBytes = 256ull * 1024 * 1024;
    size_t m_lastBatches = 0;
    uint64_t m_lastPeak = 0;
};

}   // namespace shockwave
