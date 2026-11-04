// A project's asset graph and level costs: see GameFramework/LevelAssets.h.
#include <GameFramework/LevelAssets.h>

#include <Assets/AssetFolder.h>
#include <Assets/Assets.h>
#include <Engine/AssetReferences.h>
#include <Engine/Components.h>
#include <Projects/Diagnostic.h>
#include <Projects/Project.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <sstream>

namespace shockwave {

namespace fs = std::filesystem;

std::unique_ptr<AssetGraph>
makeProjectAssetGraph(const Project& project, const AssetFolder* folder, const Assets* assets) {
    registeredComponents();   // the Engine's providers come with its components
    auto graph = std::make_unique<AssetGraph>(project.root, project.assetFolder);
    if (folder) {
        AssetGraph* g = graph.get();
        graph->imported = [folder, g](const fs::path& file) -> std::optional<std::vector<fs::path>> {
            const std::string name = g->contentName(g->idFor(file));
            const AssetFolder::File* f = name.empty() ? nullptr : folder->find(name);
            if (!f || f->kind != AssetFolder::Kind::Model || !f->imported) return std::nullopt;
            std::vector<fs::path> files;
            for (const AssetFolder::Dependency& d : f->dependencies) files.push_back(d.path);
            return files;
        };
    }
    if (assets)
        graph->knownName = [assets](AssetKind kind, std::string_view name) {
            switch (kind) {
            case AssetKind::Texture: return assets->findTexture(name).has_value();
            case AssetKind::Model: return assets->findModel(name).valid();
            case AssetKind::Material: return assets->findMaterial(name).valid();
            default: return assets->findMesh(name).valid();
            }
        };
    return graph;
}

std::vector<AssetCost>
assetCosts(AssetGraph& graph, std::string_view root, const AssetFolder* folder, const Assets* assets) {
    graph.add(root);
    std::vector<AssetCost> costs;
    for (const std::string& id : graph.closure(root)) {
        const AssetNode* node = graph.find(id);
        if (!node || !node->exists || node->builtin) continue;
        AssetCost cost{id, node->kind, node->bytes, 0};
        const std::string name = graph.contentName(id);
        if (const AssetFolder::File* f = folder && !name.empty() ? folder->find(name) : nullptr) {
            cost.gpuBytes = f->gpuBytes;
            for (const AssetFolder::TextureVariant& v : f->variants) cost.gpuBytes += v.gpuBytes;
            if (f->kind == AssetFolder::Kind::Model && assets)
                if (const auto model = assets->model(f->model))
                    for (const ModelMesh& mesh : model->data.meshes)
                        cost.gpuBytes += mesh.data.vertices.size() * sizeof(Vertex) +
                                         (mesh.data.indices.size() + mesh.data.lodIndices.size()) * sizeof(uint32_t);
        }
        costs.push_back(std::move(cost));
    }
    return costs;
}

std::string sceneAssetId(const AssetGraph& graph, const fs::path& scene) { return graph.idFor(scene); }

std::string assetDependenciesJson(
    AssetGraph& graph, const std::vector<std::pair<std::string, std::string>>& roots, const AssetFolder* folder,
    const Assets* assets) {
    auto hex = [](uint64_t v) {
        char text[24];
        std::snprintf(text, sizeof(text), "\"%016llx\"", static_cast<unsigned long long>(v));
        return std::string(text);
    };
    auto str = [](std::string_view s) { return diagnosticJsonString(s); };
    std::ostringstream out;
    std::set<std::string> used;
    std::vector<Diagnostic> diagnostics;
    out << "{\n  \"format\": 1,\n  \"mode\": \"asset_dependencies\",\n  \"roots\": [";
    bool first = true;
    for (const auto& [name, id] : roots) {
        const std::vector<AssetCost> costs = assetCosts(graph, id, folder, assets);
        const AssetNode* node = graph.find(id);
        uint64_t cpu = 0, gpu = 0;
        for (const AssetCost& c : costs) {
            cpu += c.cpuBytes;
            gpu += c.gpuBytes;
        }
        const std::vector<std::string> closure = graph.closure(id);
        used.insert(closure.begin(), closure.end());
        for (Diagnostic& d : graph.diagnostics(id)) {
            const bool seen = std::any_of(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic& o) {
                return o.code == d.code && o.file == d.file && o.line == d.line && o.message == d.message;
            });
            if (!seen) diagnostics.push_back(std::move(d));
        }
        out << (first ? "\n" : ",\n") << "    {\"name\": " << str(name) << ", \"root\": " << str(id)
            << ", \"closure_hash\": " << hex(node ? node->closureHash : 0) << ", \"cpu_bytes\": " << cpu
            << ", \"gpu_bytes\": " << gpu << ", \"assets\": [";
        for (size_t i = 0; i < closure.size(); ++i) out << (i ? ", " : "") << str(closure[i]);
        out << "]}";
        first = false;
    }
    out << "\n  ],\n  \"assets\": [";
    first = true;
    for (const std::string& id : used) {
        const AssetNode& n = *graph.find(id);
        out << (first ? "\n" : ",\n") << "    {\"id\": " << str(id) << ", \"kind\": " << str(assetKindName(n.kind))
            << ", \"exists\": " << (n.exists ? "true" : "false") << ", \"bytes\": " << n.bytes
            << ", \"hash\": " << hex(n.hash) << ", \"closure_hash\": " << hex(n.closureHash) << ", \"edges\": [";
        for (size_t i = 0; i < n.edges.size(); ++i) {
            const AssetEdge& e = n.edges[i];
            out << (i ? ", " : "") << "{\"to\": " << str(e.to) << ", \"via\": " << str(e.via)
                << ", \"line\": " << e.line << ", \"required\": " << (e.required ? "true" : "false") << "}";
        }
        out << "]}";
        first = false;
    }
    bool ok = true;
    out << "\n  ],\n  \"diagnostics\": [";
    for (size_t i = 0; i < diagnostics.size(); ++i) {
        ok = ok && !diagnostics[i].error();
        out << (i ? ",\n    " : "\n    ") << diagnosticJson(diagnostics[i]);
    }
    out << (diagnostics.empty() ? "" : "\n  ") << "],\n  \"ok\": " << (ok ? "true" : "false") << "\n}\n";
    return out.str();
}

}   // namespace shockwave
