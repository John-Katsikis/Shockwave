#include <Engine/Prefab.h>

#include "PrefabDetail.h"

#include <Assets/Assets.h>
#include <Core/Log.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Engine/SceneFile.h>

#include <algorithm>
#include <fstream>

namespace shockwave {

namespace fs = std::filesystem;

namespace {

SW_LOG_CATEGORY(LogPrefab);

// Entity references inside a prefab point at the prefab's own world; a copy needs them
// pointing at its own entities. map holds prefab entity index -> copy.
using EntityMap = std::unordered_map<uint32_t, Entity>;

Entity mapped(const PrefabAsset& prefab, const EntityMap& map, Entity e) {
    if (!prefab.world.alive(e)) return kNullEntity;
    const auto it = map.find(e.index);
    return it != map.end() ? it->second : kNullEntity;
}

void remapComponent(const ComponentInfo& info, void* data, const PrefabAsset& prefab, const EntityMap& map) {
    for (const Field& f : info.fields) {
        if (f.type != FieldType::Entity) continue;
        Entity& link = *reinterpret_cast<Entity*>(static_cast<char*>(data) + f.offset);
        link = mapped(prefab, map, link);
    }
}

// Creates the prefab's non-root entities under root, as parts.
void makeParts(World& world, Entity root, const PrefabAsset& prefab, EntityMap& map) {
    map[prefab.root.index] = root;
    std::vector<std::pair<Entity, Entity>> parts;   // prefab entity, copy
    prefab.world.forEachEntity([&](Entity p) {
        if (p == prefab.root) return;
        const Entity copy = world.create();
        map[p.index] = copy;
        parts.emplace_back(p, copy);
    });
    const std::vector<ComponentInfo>& components = registeredComponents();
    for (const auto& [p, copy] : parts) {
        for (const ComponentInfo& info : components) {
            if (!info.has(prefab.world, p)) continue;
            info.copy(prefab.world, p, world, copy);
            remapComponent(info, info.get(world, copy), prefab, map);
        }
        world.add<PrefabPart>(copy);
        if (!world.has<Parent>(copy) || !world.alive(world.get<Parent>(copy)->entity)) world.add<Parent>(copy, {root});
    }
}

// Gives root the prefab root's components, except the fields in keep.
void applyRoot(
    World& world, Entity root, const PrefabAsset& prefab, const EntityMap& map,
    const std::unordered_set<std::string>* keep, const Assets* assets) {
    const uint32_t instanceId = componentId<PrefabInstance>();
    const uint32_t parentId = componentId<Parent>();
    for (const ComponentInfo& info : registeredComponents()) {
        if (info.id == instanceId || info.id == parentId || !info.has(prefab.world, prefab.root)) continue;
        if (!info.has(world, root)) {
            info.copy(prefab.world, prefab.root, world, root);
            remapComponent(info, info.get(world, root), prefab, map);
            continue;
        }
        char* to = static_cast<char*>(info.get(world, root));
        const char* from = static_cast<const char*>(info.getConst(prefab.world, prefab.root));
        for (const Field& f : info.fields) {
            if (keep && keep->count(info.name + "." + f.name)) continue;
            if (f.type == FieldType::Entity) {
                *reinterpret_cast<Entity*>(to + f.offset) =
                    mapped(prefab, map, *reinterpret_cast<const Entity*>(from + f.offset));
            } else {
                detail::copyFieldValue(f, to + f.offset, from + f.offset, assets);
            }
        }
    }
}

// Parts made by the copy at root: descendants marked as parts whose nearest unmarked
// ancestor is root. Entities added under a part by hand aren't parts.
std::vector<Entity> partsOf(const World& world, Entity root) {
    std::vector<Entity> parts;
    for (Entity e : subtree(world, root)) {
        if (!(e == root) && world.has<PrefabPart>(e) && prefabRootOf(world, e) == root) parts.push_back(e);
    }
    return parts;
}

// "Component.field" for every field the copy at root holds differently from prefab, and
// for every field of components the prefab root doesn't have. Without a prefab, all of
// the root's fields.
std::unordered_set<std::string>
overridesOf(const World& world, Entity root, const PrefabAsset* prefab, const Assets* assets) {
    std::unordered_set<std::string> keep;
    const uint32_t instanceId = componentId<PrefabInstance>();
    for (const ComponentInfo& info : registeredComponents()) {
        if (info.id == instanceId || !info.has(world, root)) continue;
        const bool own = !prefab || !info.has(prefab->world, prefab->root);
        for (const Field& f : info.fields) {
            if (own || detail::differsFromPrefab(world, root, info, f, *prefab, assets))
                keep.insert(info.name + "." + f.name);
        }
    }
    return keep;
}

void remake(World& world, Entity root, const PrefabAsset* old, std::shared_ptr<const PrefabAsset> now) {
    const Assets* assets = prefabs().assets();
    const std::unordered_set<std::string> keep = overridesOf(world, root, old, assets);
    // What the old prefab gave the root and the new one doesn't, unless the copy changed it.
    if (old) {
        for (const ComponentInfo& info : registeredComponents()) {
            if (info.id == componentId<PrefabInstance>() || !info.has(old->world, old->root) ||
                info.has(now->world, now->root) || !info.has(world, root)) {
                continue;
            }
            const bool changed = std::any_of(info.fields.begin(), info.fields.end(), [&](const Field& f) {
                return keep.count(info.name + "." + f.name) > 0;
            });
            if (!changed) info.remove(world, root);
        }
    }
    // Entities added by hand under a part go back under the part with the same name.
    std::vector<Entity> parts = partsOf(world, root);
    std::vector<std::pair<Entity, std::string>> adopted;
    for (Entity part : parts) {
        for (Entity child : childrenOf(world, part)) {
            if (world.has<PrefabPart>(child)) continue;
            const Name* name = world.get<Name>(part);
            adopted.emplace_back(child, name ? name->value : std::string());
        }
    }
    for (Entity part : parts) world.destroy(part);
    detail::expandPrefab(world, root, std::move(now), &keep, assets);
    for (const auto& [child, partName] : adopted) {
        Entity home = root;
        for (Entity part : partsOf(world, root)) {
            const Name* name = world.get<Name>(part);
            if (name && !partName.empty() && name->value == partName) {
                home = part;
                break;
            }
        }
        setParent(world, child, home, false);
    }
}

// A world holding e and everything under it, e first; references outside it are cleared.
World copySubtree(const World& world, Entity e, std::unordered_map<uint32_t, Entity>& map) {
    World out;
    const std::vector<Entity> all = subtree(world, e);
    for (Entity original : all) map[original.index] = out.create();
    for (Entity original : all) {
        const Entity copy = map[original.index];
        for (const ComponentInfo& info : registeredComponents()) {
            if (!info.has(world, original)) continue;
            info.copy(world, original, out, copy);
            char* data = static_cast<char*>(info.get(out, copy));
            for (const Field& f : info.fields) {
                if (f.type != FieldType::Entity) continue;
                Entity& link = *reinterpret_cast<Entity*>(data + f.offset);
                const auto it = map.find(link.index);
                link = world.alive(link) && it != map.end() ? it->second : kNullEntity;
            }
        }
        if (world.has<PrefabPart>(original)) out.add<PrefabPart>(copy);
    }
    const Entity root = map[e.index];
    out.remove<Parent>(root);
    return out;
}

bool writePrefabFile(const World& world, const Assets& assets, const fs::path& file, std::string& error) {
    SceneFileHooks hooks;
    hooks.prefab = true;
    return saveScene(world, assets, file.string(), error, hooks);
}

}   // namespace

// ---- Library ------------------------------------------------------------------------------

PrefabLibrary& prefabs() {
    static PrefabLibrary library;
    return library;
}

void PrefabLibrary::setup(fs::path contentFolder, const Assets* assets) {
    std::lock_guard lock(m_mutex);
    for (auto& [key, entry] : m_entries) {
        if (entry.asset) const_cast<PrefabAsset&>(*entry.asset).stale = true;
    }
    m_entries.clear();
    m_contentFolder = std::move(contentFolder);
    m_assets = assets;
}

fs::path PrefabLibrary::contentFolder() const {
    std::lock_guard lock(m_mutex);
    return m_contentFolder;
}

const Assets* PrefabLibrary::assets() const {
    std::lock_guard lock(m_mutex);
    return m_assets;
}

std::string PrefabLibrary::key(std::string_view path) const {
    fs::path p = fs::path(path).lexically_normal();
    if (p.is_absolute()) {
        std::lock_guard lock(m_mutex);
        const fs::path inside = p.lexically_relative(m_contentFolder);
        if (!inside.empty() && *inside.begin() != "..") p = inside;
    }
    return p.generic_string();
}

fs::path PrefabLibrary::file(std::string_view path) const {
    const fs::path p(path);
    if (p.is_absolute()) return p;
    std::lock_guard lock(m_mutex);
    return m_contentFolder / key(path);
}

std::string PrefabLibrary::pathOf(const fs::path& file) const { return key(fs::absolute(file).string()); }

std::shared_ptr<const PrefabAsset> PrefabLibrary::load(std::string_view path, std::string* error) {
    std::lock_guard lock(m_mutex);
    static const Assets none;
    return loadLocked(key(path), m_assets ? *m_assets : none, error);
}

std::shared_ptr<const PrefabAsset>
PrefabLibrary::load(std::string_view path, const Assets& assets, std::string* error) {
    std::lock_guard lock(m_mutex);
    return loadLocked(key(path), assets, error);
}

std::shared_ptr<const PrefabAsset>
PrefabLibrary::loadLocked(const std::string& name, const Assets& assets, std::string* error) {
    if (const auto it = m_entries.find(name); it != m_entries.end()) {
        if (!it->second.asset && error) *error = it->second.error;
        return it->second.asset;
    }
    if (std::find(m_loading.begin(), m_loading.end(), name) != m_loading.end()) {
        // Not cached: the prefab at the top of the chain loads fine without this copy.
        std::string chain;
        for (auto it = std::find(m_loading.begin(), m_loading.end(), name); it != m_loading.end(); ++it)
            chain += *it + " -> ";
        if (error) *error = "it contains itself (" + chain + name + ")";
        return nullptr;
    }

    Entry entry;
    const fs::path path = m_contentFolder / name;
    std::error_code ec;
    entry.modified = fs::exists(path, ec) ? fs::last_write_time(path, ec) : fs::file_time_type::min();
    auto asset = std::make_shared<PrefabAsset>();
    asset->path = name;
    std::ifstream in(path);
    if (!in) {
        entry.error = "can't open " + path.string();
    } else if (fs::path(name).extension() != ".swprefab") {
        entry.error = name + " isn't a .swprefab file";
    } else {
        m_loading.push_back(name);
        SceneFileHooks hooks;
        hooks.readHeader = [](const std::string&, std::istream&) {};
        std::string readError;
        const bool ok = readScene(asset->world, assets, in, name, readError, hooks, &asset->warnings);
        m_loading.pop_back();
        if (!ok) entry.error = readError;
    }
    if (entry.error.empty()) {
        World& world = asset->world;
        asset->root = world.entityAt(0);
        if (!asset->root.valid()) entry.error = name + " has no entities; the first one is the prefab's root";
    }
    if (!entry.error.empty()) {
        if (error) *error = entry.error;
        m_entries[name] = std::move(entry);
        return nullptr;
    }

    World& world = asset->world;
    const Entity root = asset->root;
    world.remove<Parent>(root);
    // A root that is itself a prefab copy makes this a variant: its parts are this prefab's
    // own now, so it no longer needs to say where they came from.
    world.remove<PrefabInstance>(root);
    // Everything hangs under the root; ones the file left at the top level are attached.
    world.forEachEntity([&](Entity e) {
        if (!(e == root) && !parentOf(world, e).valid()) world.add<Parent>(e, {root});
    });
    world.each<const PrefabInstance>([&](const PrefabInstance& instance) {
        if (instance.path.empty()) return;
        entry.uses.push_back(key(instance.path));
        if (instance.source)
            entry.uses.insert(entry.uses.end(), instance.source->uses.begin(), instance.source->uses.end());
    });
    std::sort(entry.uses.begin(), entry.uses.end());
    entry.uses.erase(std::unique(entry.uses.begin(), entry.uses.end()), entry.uses.end());
    asset->uses = entry.uses;
    entry.asset = asset;
    m_entries[name] = std::move(entry);
    return asset;
}

std::vector<std::string> PrefabLibrary::reload(std::string_view path) {
    std::lock_guard lock(m_mutex);
    const std::string name = key(path);
    // Forget it and everything that uses it, then load them again; loading one that
    // uses another loads that one first.
    std::vector<std::string> names{name};
    for (const auto& [other, entry] : m_entries) {
        if (std::find(entry.uses.begin(), entry.uses.end(), name) != entry.uses.end()) names.push_back(other);
    }
    for (const std::string& n : names) {
        if (const auto it = m_entries.find(n); it != m_entries.end()) {
            if (it->second.asset) const_cast<PrefabAsset&>(*it->second.asset).stale = true;
            m_entries.erase(it);
        }
    }
    static const Assets none;
    for (const std::string& n : names) {
        std::string error;
        if (!loadLocked(n, m_assets ? *m_assets : none, &error))
            SW_LOG(LogPrefab, Warning, "prefab {}: {}", n, error);
        else
            SW_LOG(LogPrefab, Info, "reloaded prefab {}", n);
    }
    return names;
}

std::vector<std::string> PrefabLibrary::poll(double frameDt, double interval) {
    std::vector<std::string> changed;
    {
        std::lock_guard lock(m_mutex);
        m_pollTimer += frameDt;
        if (m_pollTimer < interval) return {};
        m_pollTimer = 0.0;
        for (const auto& [name, entry] : m_entries) {
            const fs::path path = m_contentFolder / name;
            std::error_code ec;
            const auto modified = fs::exists(path, ec) ? fs::last_write_time(path, ec) : fs::file_time_type::min();
            if (!ec && modified != entry.modified) changed.push_back(name);
        }
    }
    std::vector<std::string> reloaded;
    for (const std::string& name : changed) {
        if (std::find(reloaded.begin(), reloaded.end(), name) != reloaded.end()) continue;
        for (std::string& n : reload(name)) reloaded.push_back(std::move(n));
    }
    return reloaded;
}

void PrefabLibrary::clear() {
    std::lock_guard lock(m_mutex);
    for (auto& [key, entry] : m_entries) {
        if (entry.asset) const_cast<PrefabAsset&>(*entry.asset).stale = true;
    }
    m_entries.clear();
}

// ---- Copies -------------------------------------------------------------------------------

namespace detail {

void expandPrefab(
    World& world, Entity root, std::shared_ptr<const PrefabAsset> prefab, const std::unordered_set<std::string>* keep,
    const Assets* assets) {
    EntityMap map;
    makeParts(world, root, *prefab, map);
    applyRoot(world, root, *prefab, map, keep, assets);
    PrefabInstance& instance = world.has<PrefabInstance>(root)
                                   ? *world.get<PrefabInstance>(root)
                                   : world.add<PrefabInstance>(root, {prefab->path, nullptr, {}});
    instance.source = std::move(prefab);
}

bool differsFromPrefab(
    const World& world, Entity root, const ComponentInfo& info, const Field& f, const PrefabAsset& prefab,
    const Assets* assets) {
    const char* mine = static_cast<const char*>(info.getConst(world, root));
    const char* theirs = static_cast<const char*>(info.getConst(prefab.world, prefab.root));
    if (!mine || !theirs) return true;
    if (f.type != FieldType::Entity)
        return fieldText(f, mine + f.offset, assets) != fieldText(f, theirs + f.offset, assets);
    // A link the prefab sets points at one of the copy's own entities; any other is the copy's.
    const Entity link = *reinterpret_cast<const Entity*>(mine + f.offset);
    const Entity original = *reinterpret_cast<const Entity*>(theirs + f.offset);
    if (!prefab.world.alive(original)) return world.alive(link);
    if (!world.alive(link)) return true;
    return !(link == root || (world.has<PrefabPart>(link) && prefabRootOf(world, link) == prefabRootOf(world, root)));
}

}   // namespace detail

Entity spawnPrefab(World& world, std::string_view path, const Transform* at, Entity parent) {
    std::string error;
    auto prefab = prefabs().load(path, &error);
    if (!prefab) {
        SW_LOG(LogPrefab, Warning, "can't spawn prefab {}: {}", path, error);
        return kNullEntity;
    }
    return spawnPrefab(world, std::move(prefab), at, parent);
}

Entity spawnPrefab(World& world, std::shared_ptr<const PrefabAsset> prefab, const Transform* at, Entity parent) {
    if (!prefab) return kNullEntity;
    const Entity root = world.create();
    detail::expandPrefab(world, root, std::move(prefab), nullptr, prefabs().assets());
    if (at) world.add<Transform>(root, *at);
    if (world.alive(parent)) world.add<Parent>(root, {parent});
    return root;
}

size_t updatePrefabInstances(World& world) {
    std::vector<Entity> stale;
    world.each<const PrefabInstance>([&](Entity e, const PrefabInstance& instance) {
        if (instance.path.empty() || world.has<PrefabPart>(e)) return;   // parts follow their copy
        if (instance.source && !instance.source->stale && instance.source->path == instance.path) return;
        stale.push_back(e);
    });
    size_t remade = 0;
    for (Entity e : stale) {
        PrefabInstance* instance = world.get<PrefabInstance>(e);
        if (!instance) continue;
        auto now = prefabs().load(instance->path);
        if (!now || now == instance->source) continue;   // missing prefabs keep what they have
        const std::shared_ptr<const PrefabAsset> old = instance->source;
        // A copy pointed at another prefab starts over, apart from its root's own values.
        const PrefabAsset* before = old && old->path == now->path ? old.get() : nullptr;
        remake(world, e, before, std::move(now));
        ++remade;
    }
    return remade;
}

Entity prefabRootOf(const World& world, Entity e) {
    if (!world.alive(e)) return kNullEntity;
    Entity at = e;
    for (int depth = 0; world.has<PrefabPart>(at) && depth < kMaxHierarchyDepth; ++depth) {
        const Entity up = parentOf(world, at);
        if (!up.valid()) return kNullEntity;
        at = up;
    }
    return world.has<PrefabInstance>(at) && !world.has<PrefabPart>(at) ? at : kNullEntity;
}

Entity prefabOwnerOf(const World& world, Entity e) {
    Entity at = parentOf(world, e);
    for (int depth = 0; at.valid() && depth < kMaxHierarchyDepth; ++depth) {
        if (world.has<PrefabInstance>(at)) return at;
        at = parentOf(world, at);
    }
    return kNullEntity;
}

void unpackPrefab(World& world, Entity root) {
    if (!world.has<PrefabInstance>(root)) return;
    for (Entity e : subtree(world, root)) {
        if (!(e == root) && world.has<PrefabPart>(e) && prefabOwnerOf(world, e) == root) world.remove<PrefabPart>(e);
    }
    world.remove<PrefabInstance>(root);
}

bool savePrefab(const World& world, Entity e, const Assets& assets, const fs::path& file, std::string& error) {
    if (!world.alive(e)) {
        error = "nothing to save";
        return false;
    }
    std::unordered_map<uint32_t, Entity> map;
    World out = copySubtree(world, e, map);
    const Entity root = map[e.index];
    if (Transform* t = out.get<Transform>(root)) t->position = glm::vec3(0.0f);
    // A part of another copy is copied as what it is: an ordinary entity now.
    if (out.has<PrefabPart>(root)) {
        out.remove<PrefabPart>(root);
        for (const auto& [index, copy] : map) {
            if (out.has<PrefabPart>(copy) && !out.has<PrefabInstance>(prefabRootOf(out, copy)))
                out.remove<PrefabPart>(copy);
        }
    }
    return writePrefabFile(out, assets, file, error);
}

bool applyPrefabInstance(const World& world, Entity root, const Assets& assets, std::string& error) {
    const PrefabInstance* instance = world.get<PrefabInstance>(root);
    if (!instance || world.has<PrefabPart>(root)) {
        error = "only the root of a prefab copy in the scene can be applied";
        return false;
    }
    const auto source = instance->source;
    if (!source) {
        error = instance->path + " isn't loaded";
        return false;
    }
    std::unordered_map<uint32_t, Entity> map;
    World out = copySubtree(world, root, map);
    const Entity outRoot = map[root.index];
    // The copy's own parts become the prefab's entities again; nested copies stay copies.
    for (Entity e : subtree(world, root)) {
        if (!(e == root) && world.has<PrefabPart>(e) && prefabOwnerOf(world, e) == root)
            out.remove<PrefabPart>(map[e.index]);
    }
    out.remove<PrefabInstance>(outRoot);
    // Where the copy stands and what it's called are the copy's business.
    if (const Transform* t = source->world.get<Transform>(source->root)) {
        out.add<Transform>(outRoot, *t);
    } else {
        out.remove<Transform>(outRoot);
    }
    if (const Name* name = source->world.get<Name>(source->root)) {
        out.add<Name>(outRoot, *name);
    } else {
        out.remove<Name>(outRoot);
    }
    if (!writePrefabFile(out, assets, prefabs().file(source->path), error)) return false;
    prefabs().reload(source->path);
    return true;
}

}   // namespace shockwave
