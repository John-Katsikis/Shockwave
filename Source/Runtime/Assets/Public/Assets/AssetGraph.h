#pragma once

// The asset dependency graph (S04, docs/AssetResidency.md): which files a scene, a prefab,
// a material or a UI document needs, all the way down.
//
//   AssetGraph graph(project.root, project.assetFolder);
//   graph.add("Scenes/a.swscene");            // and everything it reaches
//   for (const std::string& id : graph.closure("Scenes/a.swscene")) ...
//   for (const Diagnostic& d : graph.diagnostics("Scenes/a.swscene")) ...
//
// Nodes are named by their path inside the project, forward slashes ("Content/ui/hud.rml"),
// or "builtin:<name>" for a mesh or texture made in code ("builtin:cube"). Each file kind
// has registered providers that read it with its own parser (the scene reader, the
// material parser, an XML tokenizer for RML) and list what it refers to. Nothing guesses
// paths out of arbitrary text. A kind without a provider is a leaf.
//
// Problems are Diagnostics with stable codes: AssetMissing, AssetPathCase,
// AssetPathOutside, AssetKindConflict, AssetCycle, AssetUnreadable (errors) and
// AssetUnknownName (a warning: a name only code registers, which a static walk can't see).
//
// Each node has a closure hash: its bytes, its kind, its providers' versions and every
// dependency's closure hash. Changing a texture changes the hash of every material,
// scene and level that reaches it, so a cache keyed by it can't go stale on timestamps.
//
// The graph keeps what it read: add() again only re-reads files whose size or time
// changed. Main thread only; providers run on it.

#include <Projects/Diagnostic.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

enum class AssetKind : uint8_t {
    Other,   // a file no provider reads: a leaf
    Scene,
    Prefab,
    Model,
    Texture,
    Material,
    Audio,
    UIDocument,
    Stylesheet,
    Font,
    Effect,
    Circuit,
    NavMesh,
    Lighting,   // a .swlighting manifest
    Reflections,   // a .swreflection manifest
    LightingData,   // the files a manifest vouches for
    PostProcess,
    Lut,
    Builtin,   // a mesh or texture made in code
};
const char* assetKindName(AssetKind kind);   // "scene", "texture", ...
// By extension, lowercase or not: ".swscene" is a Scene, ".png" a Texture. Other if unknown.
AssetKind assetKindOf(std::string_view path);

struct AssetEdge {
    std::string to;   // the target's id
    std::string via;   // what refers to it: "entity 'Crate': MeshRenderer.mesh"
    int line = 0;   // in the referring file, 1-based; 0 if unknown
    bool required = true;   // false: fine when it isn't there (a navigation mesh sidecar)
    AssetKind expect = AssetKind::Other;   // what the reference must be; Other: anything
};

struct AssetNode {
    std::string id;
    AssetKind kind = AssetKind::Other;
    bool builtin = false;   // a name registered in code, no file
    bool exists = false;
    uint64_t bytes = 0;
    uint64_t hash = 0;   // of its bytes (FNV-1a, 64 bits)
    uint64_t closureHash = 0;
    std::vector<AssetEdge> edges;
    std::string error;   // a provider couldn't read it
    // Why the reference was refused, per edge (same index): case, outside, kind.
    std::vector<std::string> refused;
    // When it was read, to notice changes.
    std::filesystem::file_time_type modified{};
    uint32_t providerVersion = 0;
};

class AssetGraph;

// What a provider gets: the file, its bytes, and calls that turn the names it finds into
// edges. Every call takes what refers to it (via) and its line, for messages.
class AssetReader {
public:
    const std::string& id() const { return m_node.id; }
    AssetKind kind() const { return m_node.kind; }
    const std::filesystem::path& file() const { return m_file; }
    std::string_view text() const { return m_text; }
    // This file's name inside Content ("fx/fire.efkefc"), or empty when it's elsewhere.
    std::string contentName() const;
    const std::filesystem::path& contentFolder() const;

    // A path inside Content ("textures/bricks.png"), as scenes and materials name assets.
    void content(
        std::string_view name, std::string via, int line = 0, AssetKind expect = AssetKind::Other,
        bool required = true);
    // A path relative to this file's folder (RML links, a model's textures). A leading /
    // means Content.
    void relative(
        std::string_view name, std::string via, int line = 0, AssetKind expect = AssetKind::Other,
        bool required = true);
    // A path on disk (what an importer opened).
    void path(
        const std::filesystem::path& file, std::string via, int line = 0, AssetKind expect = AssetKind::Other,
        bool required = true);
    // A mesh, texture or model name: a Content file when it has an extension, otherwise a
    // built-in ("cube", "white"). "none" and empty names are skipped.
    void named(std::string_view name, AssetKind kind, std::string via, int line = 0);
    // A project-relative id, for providers that know it exactly (a scene's nav sidecar).
    void
    id(std::string target, std::string via, int line = 0, AssetKind expect = AssetKind::Other, bool required = true);

    // What the importer opened for this file, when an AssetFolder imported it (a model's
    // textures and .bin buffers). Null when nothing did.
    const std::vector<std::filesystem::path>* imported() const { return m_imported ? &*m_imported : nullptr; }

private:
    friend class AssetGraph;
    AssetReader(AssetGraph& graph, AssetNode& node, std::filesystem::path file, std::string_view text)
        : m_graph(graph), m_node(node), m_file(std::move(file)), m_text(text) {}
    void add(std::string target, std::string refusal, std::string via, int line, AssetKind expect, bool required);
    AssetGraph& m_graph;
    AssetNode& m_node;
    std::filesystem::path m_file;
    std::string_view m_text;
    std::optional<std::vector<std::filesystem::path>> m_imported;
};

// Reads one file and adds its edges. False with error when the file is broken; the edges
// it found before that still count.
using AssetProvider = std::function<bool(AssetReader&, std::string& error)>;

// Registers a provider for files with these extensions (lowercase, with the dot). Several
// providers may read one kind of file (the scene reader, then navigation adding its
// sidecar); registering a name again replaces it. version goes into the closure hash: bump
// it when the provider finds different edges. Startup, main thread.
void registerAssetProvider(
    std::string name, std::vector<std::string> extensions, AssetProvider provider, uint32_t version = 1);
// The providers' names for an extension, in registration order.
std::vector<std::string> assetProviders(std::string_view extension);

class AssetGraph {
public:
    AssetGraph(std::filesystem::path projectRoot, std::filesystem::path contentFolder);

    // A name with no file: true when code registered it (Assets has a mesh "cube"). Unset:
    // every such name is known.
    std::function<bool(AssetKind kind, std::string_view name)> knownName;
    // What an importer opened for a file (AssetFolder's File::dependencies), or nullopt.
    std::function<std::optional<std::vector<std::filesystem::path>>(const std::filesystem::path&)> imported;

    // Adds a node and everything it reaches. Re-reads changed files. Returns the node.
    const AssetNode& add(std::string_view id);
    const AssetNode* find(std::string_view id) const;
    const std::map<std::string, AssetNode, std::less<>>& nodes() const { return m_nodes; }

    // The id for a file on disk, or a Content name; empty when it's outside the project.
    std::string idFor(const std::filesystem::path& file) const;
    std::string contentId(std::string_view name) const;
    std::filesystem::path pathOf(std::string_view id) const;   // on disk
    const std::filesystem::path& projectRoot() const { return m_root; }
    const std::filesystem::path& contentFolder() const { return m_content; }
    // A Content-relative name for an id under Content ("Content/a/b.png" -> "a/b.png"), or
    // empty.
    std::string contentName(std::string_view id) const;

    // The id and everything it reaches, sorted; each once.
    std::vector<std::string> closure(std::string_view id) const;
    // Problems in what the id reaches (all nodes when empty): missing files, wrong case,
    // paths leaving the project, kind conflicts, unreadable files, cycles.
    std::vector<Diagnostic> diagnostics(std::string_view id = {}) const;
    // Every cycle, each as the ids around it with the first repeated at the end.
    const std::vector<std::vector<std::string>>& cycles() const { return m_cycles; }

    // Forget everything read (the next add() reads every file again).
    void clear();
    // How many files the last add() read (the rest came from what it kept). For tests.
    size_t lastReads() const { return m_reads; }

private:
    friend class AssetReader;
    void visit(const std::string& id, std::vector<std::string>& queue);
    void finish();
    std::string caseProblem(const std::filesystem::path& relative) const;

    std::filesystem::path m_root, m_content;
    std::string m_contentPrefix;   // "Content/"
    std::map<std::string, AssetNode, std::less<>> m_nodes;
    std::vector<std::vector<std::string>> m_cycles;
    mutable std::map<std::filesystem::path, std::vector<std::string>> m_listings;   // per add()
    size_t m_reads = 0;
};

// FNV-1a, 64 bits.
uint64_t assetHash(std::string_view bytes, uint64_t seed = 14695981039346656037ull);

}   // namespace shockwave
