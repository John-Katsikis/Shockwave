// The asset dependency graph: see Assets/AssetGraph.h and docs/AssetResidency.md.
#include <Assets/AssetGraph.h>

#include <Assets/AudioClip.h>
#include <Assets/Image.h>
#include <Assets/LightingData.h>
#include <Assets/Material.h>
#include <Assets/Model.h>
#include <Assets/ReflectionData.h>
#include <Assets/TextureCompression.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

namespace shockwave {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

std::string extensionOf(std::string_view path) {
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    return lower(path.substr(dot));
}

struct ProviderEntry {
    std::string name;
    std::vector<std::string> extensions;
    AssetProvider provider;
    uint32_t version = 1;
};

std::vector<ProviderEntry>& providers() {
    static std::vector<ProviderEntry> list;
    return list;
}

void registerBuiltinProviders();

const std::vector<ProviderEntry>& allProviders() {
    registerBuiltinProviders();
    return providers();
}

bool readBytes(const fs::path& file, std::string& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

}   // namespace

uint64_t assetHash(std::string_view bytes, uint64_t seed) {
    uint64_t h = seed;
    for (const char c : bytes) {
        h ^= uint8_t(c);
        h *= 1099511628211ull;
    }
    return h;
}

const char* assetKindName(AssetKind kind) {
    switch (kind) {
    case AssetKind::Other: return "file";
    case AssetKind::Scene: return "scene";
    case AssetKind::Prefab: return "prefab";
    case AssetKind::Model: return "model";
    case AssetKind::Texture: return "texture";
    case AssetKind::Material: return "material";
    case AssetKind::Audio: return "audio";
    case AssetKind::UIDocument: return "ui document";
    case AssetKind::Stylesheet: return "stylesheet";
    case AssetKind::Font: return "font";
    case AssetKind::Effect: return "effect";
    case AssetKind::Circuit: return "circuit";
    case AssetKind::NavMesh: return "navmesh";
    case AssetKind::Lighting: return "lighting";
    case AssetKind::Reflections: return "reflections";
    case AssetKind::LightingData: return "lighting data";
    case AssetKind::PostProcess: return "post process";
    case AssetKind::Lut: return "lut";
    case AssetKind::Builtin: return "builtin";
    }
    return "file";
}

AssetKind assetKindOf(std::string_view path) {
    const std::string ext = extensionOf(path);
    if (ext.empty()) return AssetKind::Other;
    if (ext == ".swscene") return AssetKind::Scene;
    if (ext == ".swprefab") return AssetKind::Prefab;
    if (ext == ".swmaterial") return AssetKind::Material;
    if (ext == ".rml") return AssetKind::UIDocument;
    if (ext == ".rcss") return AssetKind::Stylesheet;
    if (ext == ".ttf" || ext == ".otf") return AssetKind::Font;
    if (ext == ".efkefc") return AssetKind::Effect;
    if (ext == ".swcircuit") return AssetKind::Circuit;
    if (ext == ".swnavmesh") return AssetKind::NavMesh;
    if (ext == ".swlighting") return AssetKind::Lighting;
    if (ext == ".swreflection") return AssetKind::Reflections;
    if (ext == ".swpostprocess") return AssetKind::PostProcess;
    if (ext == ".cube") return AssetKind::Lut;
    const fs::path p{std::string(path)};
    if (isAudioFile(p)) return AssetKind::Audio;
    if (isImageFile(p) || ext == ".ktx2") return AssetKind::Texture;
    if (isModelFile(p) || ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".obj") return AssetKind::Model;
    return AssetKind::Other;
}

void registerAssetProvider(
    std::string name, std::vector<std::string> extensions, AssetProvider provider, uint32_t version) {
    for (std::string& ext : extensions) ext = lower(ext);
    auto& list = providers();
    const auto it = std::find_if(list.begin(), list.end(), [&](const ProviderEntry& e) { return e.name == name; });
    ProviderEntry entry{std::move(name), std::move(extensions), std::move(provider), version};
    if (it != list.end())
        *it = std::move(entry);
    else
        list.push_back(std::move(entry));
}

std::vector<std::string> assetProviders(std::string_view extension) {
    std::vector<std::string> names;
    const std::string ext = lower(extension);
    for (const ProviderEntry& e : allProviders())
        if (std::find(e.extensions.begin(), e.extensions.end(), ext) != e.extensions.end()) names.push_back(e.name);
    return names;
}

// ---- AssetReader -----------------------------------------------------------------------

void AssetReader::add(
    std::string target, std::string refusal, std::string via, int line, AssetKind expect, bool required) {
    if (target.empty() && refusal.empty()) return;
    // The same reference twice from one place adds nothing.
    for (size_t i = 0; i < m_node.edges.size(); ++i) {
        const AssetEdge& e = m_node.edges[i];
        if (e.to == target && e.via == via && m_node.refused[i] == refusal) return;
    }
    m_node.edges.push_back({std::move(target), std::move(via), line, required, expect});
    m_node.refused.push_back(std::move(refusal));
}

std::string AssetReader::contentName() const { return m_graph.contentName(m_node.id); }

const fs::path& AssetReader::contentFolder() const { return m_graph.contentFolder(); }

void AssetReader::content(std::string_view name, std::string via, int line, AssetKind expect, bool required) {
    if (name.empty()) return;
    const std::string target = m_graph.contentId(name);
    if (target.empty())
        return add(
            std::string(name), "\"" + std::string(name) + "\" leads outside the project", std::move(via), line, expect,
            required);
    add(target, {}, std::move(via), line, expect, required);
}

void AssetReader::relative(std::string_view name, std::string via, int line, AssetKind expect, bool required) {
    if (name.empty()) return;
    if (name.front() == '/') return content(name.substr(1), std::move(via), line, expect, required);
    path(m_file.parent_path() / fs::path(std::string(name)), std::move(via), line, expect, required);
}

void AssetReader::path(const fs::path& onDisk, std::string via, int line, AssetKind expect, bool required) {
    const std::string target = m_graph.idFor(onDisk);
    if (target.empty())
        return add(
            onDisk.generic_string(), "\"" + onDisk.generic_string() + "\" is outside the project", std::move(via), line,
            expect, required);
    add(target, {}, std::move(via), line, expect, required);
}

void AssetReader::named(std::string_view name, AssetKind type, std::string via, int line) {
    if (name.empty() || name == "none") return;
    const std::string_view leaf =
        name.substr(name.find_last_of('/') == std::string_view::npos ? 0 : name.find_last_of('/'));
    if (leaf.find('.') != std::string_view::npos)
        return content(name, std::move(via), line, type == AssetKind::Builtin ? AssetKind::Other : type);
    add("builtin:" + std::string(name), {}, std::move(via), line, type, true);
}

void AssetReader::id(std::string target, std::string via, int line, AssetKind expect, bool required) {
    add(std::move(target), {}, std::move(via), line, expect, required);
}

// ---- AssetGraph ------------------------------------------------------------------------

AssetGraph::AssetGraph(fs::path projectRoot, fs::path contentFolder)
    : m_root(fs::absolute(projectRoot).lexically_normal()), m_content(fs::absolute(contentFolder).lexically_normal()) {
    if (!m_root.empty() && m_root.filename().empty()) m_root = m_root.parent_path();
    if (!m_content.empty() && m_content.filename().empty()) m_content = m_content.parent_path();
    const fs::path rel = m_content.lexically_relative(m_root);
    m_contentPrefix = rel.empty() || rel == "." ? std::string() : rel.generic_string() + "/";
}

std::string AssetGraph::idFor(const fs::path& file) const {
    const fs::path normal = fs::absolute(file).lexically_normal();
    const fs::path rel = normal.lexically_relative(m_root);
    if (rel.empty() || *rel.begin() == "..") return {};
    return rel.generic_string();
}

std::string AssetGraph::contentId(std::string_view name) const {
    return idFor(m_content / fs::path(std::string(name)));
}

fs::path AssetGraph::pathOf(std::string_view id) const { return m_root / fs::path(std::string(id)); }

std::string AssetGraph::contentName(std::string_view id) const {
    if (m_contentPrefix.empty() || id.substr(0, m_contentPrefix.size()) != m_contentPrefix) return {};
    return std::string(id.substr(m_contentPrefix.size()));
}

const AssetNode* AssetGraph::find(std::string_view id) const {
    const auto it = m_nodes.find(id);
    return it == m_nodes.end() ? nullptr : &it->second;
}

void AssetGraph::clear() {
    m_nodes.clear();
    m_cycles.clear();
}

// The disk's own spelling, part by part: a case-insensitive disk would accept any case,
// and the mistake would only show where the disk isn't.
std::string AssetGraph::caseProblem(const fs::path& relative) const {
    fs::path here = m_root;
    std::error_code ec;
    for (const fs::path& part : relative) {
        const std::string name = part.string();
        if (name == "." || name == "..") {
            here /= part;
            continue;
        }
        auto [it, inserted] = m_listings.try_emplace(here);
        if (inserted)
            for (const auto& entry : fs::directory_iterator(here, ec))
                it->second.push_back(entry.path().filename().string());
        const auto& listing = it->second;
        if (std::find(listing.begin(), listing.end(), name) == listing.end()) {
            for (const std::string& other : listing)
                if (lower(other) == lower(name)) return "the disk spells \"" + name + "\" as \"" + other + "\"";
            return {};   // missing: reported as such
        }
        here /= part;
    }
    return {};
}

void AssetGraph::visit(const std::string& id, std::vector<std::string>& queue) {
    AssetNode& node = m_nodes[id];
    const bool fresh = node.id.empty();
    node.id = id;
    if (id.rfind("builtin:", 0) == 0) {
        node.kind = AssetKind::Builtin;
        node.builtin = true;
        node.exists = true;
        node.hash = assetHash(id);
        return;
    }
    node.kind = assetKindOf(id);
    const fs::path file = pathOf(id);
    std::error_code ec;
    const bool exists = fs::is_regular_file(file, ec);
    const auto modified = exists ? fs::last_write_time(file, ec) : fs::file_time_type{};
    const uint64_t bytes = exists ? uint64_t(fs::file_size(file, ec)) : 0;
    uint32_t version = 0;
    std::vector<const ProviderEntry*> readers;
    const std::string ext = extensionOf(id);
    for (const ProviderEntry& e : allProviders())
        if (std::find(e.extensions.begin(), e.extensions.end(), ext) != e.extensions.end()) {
            readers.push_back(&e);
            version = version * 31 + e.version;
        }
    std::optional<std::vector<fs::path>> importedFiles;
    if (exists && imported) importedFiles = imported(file);
    const bool unchanged = !fresh && node.exists == exists && node.modified == modified && node.bytes == bytes &&
                           node.providerVersion == version && !importedFiles;
    if (!unchanged) {
        ++m_reads;
        node.exists = exists;
        node.modified = modified;
        node.bytes = bytes;
        node.providerVersion = version;
        node.edges.clear();
        node.refused.clear();
        node.error.clear();
        node.hash = 0;
        std::string text;
        if (exists && !readBytes(file, text)) node.error = "can't read it";
        node.hash = assetHash(text);
        if (exists && node.error.empty()) {
            AssetReader reader(*this, node, file, text);
            reader.m_imported = std::move(importedFiles);
            for (const ProviderEntry* e : readers) {
                std::string error;
                bool ok = false;
                try {
                    ok = e->provider(reader, error);
                } catch (const std::exception& ex) {
                    error = ex.what();
                }
                if (!ok && node.error.empty()) node.error = error.empty() ? "can't read it" : error;
            }
        }
    }
    for (size_t i = 0; i < node.edges.size(); ++i)
        if (node.refused[i].empty() && !node.edges[i].to.empty()) queue.push_back(node.edges[i].to);
}

const AssetNode& AssetGraph::add(std::string_view root) {
    m_reads = 0;
    m_listings.clear();
    std::vector<std::string> queue{std::string(root)};
    std::set<std::string, std::less<>> seen;
    while (!queue.empty()) {
        std::string id = std::move(queue.back());
        queue.pop_back();
        if (!seen.insert(id).second) continue;
        visit(id, queue);
    }
    finish();
    return m_nodes.at(std::string(root));
}

// Cycles (DFS with colours) and closure hashes, over the whole graph.
void AssetGraph::finish() {
    m_cycles.clear();
    enum Colour : uint8_t { White, Grey, Black };
    std::map<std::string_view, Colour, std::less<>> colour;
    std::vector<std::string_view> stack;
    std::set<std::vector<std::string>> found;
    std::function<void(const AssetNode&)> dfs = [&](const AssetNode& node) {
        colour[node.id] = Grey;
        stack.push_back(node.id);
        for (const AssetEdge& edge : node.edges) {
            const AssetNode* next = find(edge.to);
            if (!next) continue;
            const Colour c = colour.count(next->id) ? colour[next->id] : White;
            if (c == Grey) {
                const auto from = std::find(stack.begin(), stack.end(), std::string_view(next->id));
                std::vector<std::string> cycle(from, stack.end());
                // The same cycle from another start: rotate so the smallest id leads.
                std::rotate(cycle.begin(), std::min_element(cycle.begin(), cycle.end()), cycle.end());
                cycle.push_back(cycle.front());
                if (found.insert(cycle).second) m_cycles.push_back(std::move(cycle));
            } else if (c == White) {
                dfs(*next);
            }
        }
        stack.pop_back();
        colour[node.id] = Black;
    };
    for (const auto& [id, node] : m_nodes)
        if (!colour.count(id)) dfs(node);

    // Closure hashes: a node's own hash, kind and provider version, then each target's
    // id and closure hash. A target on a cycle back to an unfinished node adds its id only.
    std::map<std::string_view, uint64_t, std::less<>> done;
    std::set<std::string_view, std::less<>> active;
    std::function<uint64_t(AssetNode&)> hashOf = [&](AssetNode& node) -> uint64_t {
        if (const auto it = done.find(node.id); it != done.end()) return it->second;
        active.insert(node.id);
        uint64_t h = assetHash(node.id, node.hash);
        const uint8_t kind = uint8_t(node.kind);
        h = assetHash(std::string_view(reinterpret_cast<const char*>(&kind), 1), h);
        h = assetHash(std::to_string(node.providerVersion), h);
        h = assetHash(node.exists ? "1" : "0", h);
        std::vector<std::pair<std::string, uint64_t>> children;
        for (const AssetEdge& edge : node.edges) {
            const auto it = m_nodes.find(edge.to);
            uint64_t child = assetHash(edge.to);
            if (it != m_nodes.end() && !active.count(edge.to)) child = hashOf(it->second);
            children.emplace_back(edge.to, child);
        }
        std::sort(children.begin(), children.end());
        children.erase(std::unique(children.begin(), children.end()), children.end());
        for (const auto& [id, child] : children) {
            h = assetHash(id, h);
            h = assetHash(std::string_view(reinterpret_cast<const char*>(&child), sizeof(child)), h);
        }
        active.erase(node.id);
        node.closureHash = h;
        done[node.id] = h;
        return h;
    };
    for (auto& [id, node] : m_nodes) hashOf(node);
}

std::vector<std::string> AssetGraph::closure(std::string_view id) const {
    std::vector<std::string> out;
    std::set<std::string, std::less<>> seen;
    std::vector<std::string> queue{std::string(id)};
    while (!queue.empty()) {
        std::string next = std::move(queue.back());
        queue.pop_back();
        const AssetNode* node = find(next);
        if (!node || !seen.insert(next).second) continue;
        out.push_back(next);
        for (const AssetEdge& edge : node->edges) {
            // An optional file that isn't there (a scene's unbaked navigation mesh) isn't needed.
            const AssetNode* target = find(edge.to);
            if (!edge.required && (!target || !target->exists)) continue;
            queue.push_back(edge.to);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Diagnostic> AssetGraph::diagnostics(std::string_view root) const {
    std::vector<Diagnostic> out;
    std::vector<std::string> ids;
    if (root.empty())
        for (const auto& [id, node] : m_nodes) ids.push_back(id);
    else
        ids = closure(root);
    const std::set<std::string, std::less<>> reach(ids.begin(), ids.end());
    m_listings.clear();
    for (const std::string& id : ids) {
        const AssetNode& node = m_nodes.at(id);
        auto make = [&](const char* code, DiagnosticSeverity severity, int line, std::string object,
                        std::string message) {
            out.push_back({code, severity, node.id, line, std::move(object), std::move(message)});
        };
        if (!node.error.empty()) make("AssetUnreadable", DiagnosticSeverity::Error, 0, {}, node.error);
        for (size_t i = 0; i < node.edges.size(); ++i) {
            const AssetEdge& edge = node.edges[i];
            const std::string& refused = node.refused[i];
            if (!refused.empty()) {
                make("AssetPathOutside", DiagnosticSeverity::Error, edge.line, edge.via, refused);
                continue;
            }
            const AssetNode* target = find(edge.to);
            if (!target) continue;
            if (target->builtin) {
                const std::string name = edge.to.substr(8);
                if (knownName && !knownName(edge.expect, name))
                    make(
                        "AssetUnknownName", DiagnosticSeverity::Warning, edge.line, edge.via,
                        std::string(assetKindName(edge.expect)) + " \"" + name +
                            "\" isn't a file or a built-in; only code that registers it at run time can provide it");
                continue;
            }
            const std::string shown = contentName(edge.to).empty() ? edge.to : contentName(edge.to);
            if (!target->exists) {
                const std::string problem = caseProblem(fs::path(edge.to));
                if (!problem.empty())
                    make(
                        "AssetPathCase", DiagnosticSeverity::Error, edge.line, edge.via,
                        "\"" + shown + "\": " + problem);
                else if (edge.required)
                    make(
                        "AssetMissing", DiagnosticSeverity::Error, edge.line, edge.via,
                        "\"" + shown + "\" doesn't exist");
                continue;
            }
            if (const std::string problem = caseProblem(fs::path(edge.to)); !problem.empty()) {
                make("AssetPathCase", DiagnosticSeverity::Error, edge.line, edge.via, "\"" + shown + "\": " + problem);
                continue;
            }
            if (edge.expect != AssetKind::Other && target->kind != edge.expect)
                make(
                    "AssetKindConflict", DiagnosticSeverity::Error, edge.line, edge.via,
                    "\"" + shown + "\" is a " + assetKindName(target->kind) + ", not a " + assetKindName(edge.expect));
        }
    }
    for (const std::vector<std::string>& cycle : m_cycles) {
        if (!reach.count(cycle.front())) continue;
        std::string chain;
        for (const std::string& id : cycle) chain += (chain.empty() ? "" : " -> ") + id;
        // Name the edge that closes the loop, so the message says where to cut it.
        const AssetNode& from = m_nodes.at(cycle[cycle.size() - 2]);
        int line = 0;
        std::string via;
        for (const AssetEdge& e : from.edges)
            if (e.to == cycle.back()) {
                line = e.line;
                via = e.via;
                break;
            }
        out.push_back(
            {"AssetCycle", DiagnosticSeverity::Error, from.id, line, via,
             "it reaches itself: " + chain + " (no asset kind can depend on itself)"});
    }
    return out;
}

// ---- Providers for the Assets module's own formats --------------------------------------

namespace {

void registerBuiltinProviders() {
    static bool done = false;
    if (done) return;
    done = true;
    // A material: its parent (another material, or a model whose material it starts from)
    // and each slot's texture.
    registerAssetProvider("material", {".swmaterial"}, [](AssetReader& r, std::string& error) {
        MaterialFile file;
        if (!parseMaterial(r.text(), file, error)) return false;
        if (!file.parent.empty()) r.content(file.parent, "parent");
        for (size_t slot = 0; slot < file.slots.size(); ++slot) {
            const MaterialSlotOverride& o = file.slots[slot];
            if (o.hasTexture && !o.texture.empty())
                r.named(o.texture, AssetKind::Texture, std::string(materialSlotName(MaterialSlot(slot))) + " texture");
        }
        return true;
    });
    // A model: what its importer opened (external textures, .bin buffers, .mtl files),
    // when an AssetFolder imported it. The .swimport sidecar is optional.
    registerAssetProvider(
        "model", {".gltf", ".glb", ".fbx", ".obj", ".dae", ".blend", ".3ds", ".ply", ".stl"},
        [](AssetReader& r, std::string&) {
            if (const auto* files = r.imported())
                for (const fs::path& f : *files) {
                    if (f == r.file()) continue;
                    const bool sidecar = f.extension() == ".swimport";
                    r.path(f, sidecar ? "import settings" : "import", 0, AssetKind::Other, !sidecar);
                }
            return true;
        });
    // Baked lighting and reflections: the generated files the manifest vouches for. The
    // manifest's scene line points back at the scene; that's provenance, not a use.
    registerAssetProvider("lighting", {".swlighting"}, [](AssetReader& r, std::string& error) {
        LightingManifest manifest;
        if (!parseLightingManifest(r.text(), r.id(), manifest, error)) return false;
        for (const LightingResource& res : manifest.resources)
            r.content(res.path, lightingResourceKindName(res.kind), 0, AssetKind::Other);
        return true;
    });
    registerAssetProvider("reflections", {".swreflection"}, [](AssetReader& r, std::string& error) {
        ReflectionManifest manifest;
        if (!parseReflectionManifest(r.text(), r.id(), manifest, error)) return false;
        for (const ReflectionProbeRecord& probe : manifest.probes)
            r.content(probe.path, "probe cube", 0, AssetKind::Texture);
        return true;
    });
}

}   // namespace

}   // namespace shockwave
