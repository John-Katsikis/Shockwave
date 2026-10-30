#include <Engine/SceneFile.h>

#include "PrefabDetail.h"

#include <Engine/Components.h>
#include <Engine/Hierarchy.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace shockwave {

namespace {

constexpr int kFormatVersion = 2;

// Six significant digits, and no "-0" in files.
struct Num {
    float v;
};
std::ostream& operator<<(std::ostream& out, Num n) { return out << (n.v == 0.0f ? 0.0f : n.v); }

void writeFloats(std::ostream& out, const float* v, int n) {
    for (int i = 0; i < n; ++i) out << ' ' << Num{v[i]};
}

bool readFloats(std::istream& in, float* v, int n) {
    for (int i = 0; i < n; ++i) {
        if (!(in >> v[i])) return false;
    }
    return true;
}

// Asset names are written bare unless they need quotes (spaces, say), so files stay
// readable and older builds, which read bare words, still open them.
void writeName(std::ostream& out, const std::string& name) {
    const bool plain = !name.empty() && name.find_first_of(" \t\"\\#") == std::string::npos;
    if (plain) {
        out << ' ' << name;
    } else {
        out << ' ' << std::quoted(name);
    }
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

bool parseInt(const std::string& text, int& value) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && end == text.data() + text.size();
}

// entityRefs maps an entity index to how other entities refer to it in the file.

void writeField(
    std::ostream& out, const Field& f, const void* data, const Assets& assets, const World& world,
    const std::vector<std::string>& entityRefs) {
    switch (f.type) {
    case FieldType::Bool: out << ' ' << (*static_cast<const bool*>(data) ? 1 : 0); break;
    case FieldType::Enum: {
        int value;
        std::memcpy(&value, data, sizeof(value));
        if (value >= 0 && size_t(value) < f.options.size()) {
            writeName(out, f.options[size_t(value)]);
        } else {
            out << ' ' << value;
        }
        break;
    }
    case FieldType::Int: out << ' ' << *static_cast<const int*>(data); break;
    case FieldType::Float: writeFloats(out, static_cast<const float*>(data), 1); break;
    case FieldType::Vec2: writeFloats(out, static_cast<const float*>(data), 2); break;
    case FieldType::Vec3: writeFloats(out, static_cast<const float*>(data), 3); break;
    case FieldType::Color: writeFloats(out, static_cast<const float*>(data), 4); break;
    case FieldType::Rotation: {
        const glm::vec3 euler = eulerDegrees(*static_cast<const glm::quat*>(data));
        writeFloats(out, &euler.x, 3);
        break;
    }
    case FieldType::AudioClip:
    case FieldType::UIDocument:
    case FieldType::String: out << ' ' << std::quoted(*static_cast<const std::string*>(data)); break;
    case FieldType::Mesh: {
        const std::string name = assets.meshName(*static_cast<const MeshHandle*>(data));
        writeName(out, name.empty() ? "none" : name);
        break;
    }
    case FieldType::Model: {
        const auto name = assets.modelName(*static_cast<const ModelHandle*>(data));
        writeName(out, name.empty() ? "none" : name);
        break;
    }
    case FieldType::Texture: {
        const std::string name = assets.textureName(*static_cast<const TextureHandle*>(data));
        writeName(out, name.empty() ? "white" : name);
        break;
    }
    case FieldType::Material: {
        const std::string name = assets.materialName(*static_cast<const MaterialHandle*>(data));
        writeName(out, name.empty() ? "none" : name);
        break;
    }
    case FieldType::MaterialOverrides:
        // Pairs: the model's material name, then the material asset.
        for (const MaterialOverride& o : *static_cast<const MaterialOverrides*>(data)) {
            const std::string name = assets.materialName(o.material);
            if (name.empty()) continue;
            out << ' ' << std::quoted(o.part) << ' ' << std::quoted(name);
        }
        break;
    case FieldType::Entity: {
        const Entity e = *static_cast<const Entity*>(data);
        out << ' ' << (world.alive(e) && e.index < entityRefs.size() ? entityRefs[e.index] : "none");
        break;
    }
    }
}

// Returns false if the value doesn't parse; unknown asset names load as defaults with a warning.
bool readField(std::istream& in, const Field& f, void* data, const Assets& assets, std::vector<std::string>& warnings) {
    switch (f.type) {
    case FieldType::Bool: {
        std::string word;
        if (!(in >> word)) return false;
        word = lower(word);
        if (word == "1" || word == "true" || word == "yes")
            *static_cast<bool*>(data) = true;
        else if (word == "0" || word == "false" || word == "no")
            *static_cast<bool*>(data) = false;
        else
            return false;
        return true;
    }
    case FieldType::Enum: {
        // An option's name ("Box", "Convex model") or its number.
        std::string word;
        if (!(in >> std::quoted(word))) return false;
        int value = -1;
        if (!parseInt(word, value)) {
            for (size_t i = 0; i < f.options.size(); ++i) {
                if (lower(f.options[i]) == lower(word)) value = int(i);
            }
            if (value < 0) return false;
        }
        if (!f.options.empty() && (value < 0 || size_t(value) >= f.options.size())) return false;
        std::memcpy(data, &value, sizeof(value));
        return true;
    }
    case FieldType::Int: return static_cast<bool>(in >> *static_cast<int*>(data));
    case FieldType::Float: return readFloats(in, static_cast<float*>(data), 1);
    case FieldType::Vec2: return readFloats(in, static_cast<float*>(data), 2);
    case FieldType::Vec3: return readFloats(in, static_cast<float*>(data), 3);
    case FieldType::Color: {
        float* c = static_cast<float*>(data);
        if (!readFloats(in, c, 3)) return false;
        if (!(in >> c[3])) c[3] = 1.0f;   // alpha is optional
        return true;
    }
    case FieldType::Rotation: {
        glm::vec3 euler;
        if (!readFloats(in, &euler.x, 3)) return false;
        *static_cast<glm::quat*>(data) = fromEulerDegrees(euler);
        return true;
    }
    case FieldType::AudioClip:
    case FieldType::UIDocument:
    case FieldType::String: return static_cast<bool>(in >> std::quoted(*static_cast<std::string*>(data)));
    case FieldType::Mesh: {
        std::string name;
        if (!(in >> std::quoted(name))) return false;
        const MeshHandle mesh = assets.findMesh(name);
        if (!mesh.valid() && name != "none") warnings.push_back("unknown mesh '" + name + "'");
        *static_cast<MeshHandle*>(data) = mesh;
        return true;
    }
    case FieldType::Model: {
        std::string name;
        if (!(in >> std::quoted(name))) return false;
        const auto handle = assets.findModel(name);
        if (!handle.valid() && name != "none") warnings.push_back("unknown model '" + name + "'");
        *static_cast<ModelHandle*>(data) = handle;
        return true;
    }
    case FieldType::Texture: {
        std::string name;
        if (!(in >> std::quoted(name))) return false;
        const auto texture = assets.findTexture(name);
        if (!texture) warnings.push_back("unknown texture '" + name + "'");
        *static_cast<TextureHandle*>(data) = texture.value_or(TextureHandle{});
        return true;
    }
    case FieldType::Material: {
        std::string name;
        if (!(in >> std::quoted(name))) return false;
        const MaterialHandle handle = assets.findMaterial(name);
        if (!handle.valid() && name != "none") warnings.push_back("unknown material '" + name + "'");
        *static_cast<MaterialHandle*>(data) = handle;
        return true;
    }
    case FieldType::MaterialOverrides: {
        MaterialOverrides overrides;
        std::string part, name;
        while (in >> std::quoted(part)) {
            if (!(in >> std::quoted(name))) return false;
            const MaterialHandle handle = assets.findMaterial(name);
            if (!handle.valid()) {
                warnings.push_back("unknown material '" + name + "' for part '" + part + "'");
                continue;
            }
            if (findMaterialOverride(overrides, part)) return false;   // a part given twice
            overrides.push_back({part, handle});
        }
        *static_cast<MaterialOverrides*>(data) = std::move(overrides);
        in.clear(std::ios::eofbit);
        return true;
    }
    case FieldType::Entity: return false;   // needs the whole file; loadScene handles it
    }
    return false;
}

// How a field's value is written, for error messages and the schema.
std::string valueSyntax(const Field& f) {
    switch (f.type) {
    case FieldType::Bool: return "1 or 0 (true/false also accepted)";
    case FieldType::Int: return "whole number";
    case FieldType::Enum: {
        std::string text = "one of:";
        for (size_t i = 0; i < f.options.size(); ++i) {
            text += (i ? ", " : " ") +
                    (f.options[i].find(' ') == std::string::npos ? f.options[i] : "\"" + f.options[i] + "\"");
        }
        return text + " (or its number, counting from 0)";
    }
    case FieldType::Float: return "number";
    case FieldType::Vec2: return "x y";
    case FieldType::Vec3: return "x y z";
    case FieldType::Color: return "r g b [a], linear 0..1, alpha optional";
    case FieldType::Rotation: return "pitch yaw roll in degrees (applied X, then Y, then Z)";
    case FieldType::String: return "quoted text";
    case FieldType::AudioClip: return "quoted audio clip path inside Content (.wav or .ogg)";
    case FieldType::Mesh: return "mesh name, or none";
    case FieldType::Texture: return "texture name (white when unset)";
    case FieldType::Model: return "model path inside the asset folder, or none";
    case FieldType::Entity: return "quoted entity name, or none (a block number counting from 0 also works)";
    case FieldType::UIDocument: return "quoted .rml document path inside Content";
    case FieldType::Material: return "quoted .swmaterial path inside Content, or none";
    case FieldType::MaterialOverrides:
        return "pairs of quoted names: the model's material, then the .swmaterial drawn in its place";
    }
    return {};
}

const Field* findField(const ComponentInfo& info, const std::string& name) {
    for (const Field& f : info.fields) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

bool restIsEmpty(std::istream& in) {
    std::string extra;
    return !(in >> extra);
}

}   // namespace

namespace detail {

std::string fieldText(const Field& f, const void* data, const Assets* assets) {
    static const Assets none;
    static const World noWorld;
    std::ostringstream out;
    out << std::setprecision(6);
    writeField(out, f, data, assets ? *assets : none, noWorld, {});
    return out.str();
}

void copyFieldValue(const Field& f, void* to, const void* from, const Assets* assets) {
    switch (f.type) {
    case FieldType::Bool: *static_cast<bool*>(to) = *static_cast<const bool*>(from); return;
    case FieldType::Int:
    case FieldType::Enum: std::memcpy(to, from, sizeof(int)); return;
    case FieldType::Float: *static_cast<float*>(to) = *static_cast<const float*>(from); return;
    case FieldType::Vec2: *static_cast<glm::vec2*>(to) = *static_cast<const glm::vec2*>(from); return;
    case FieldType::Vec3: *static_cast<glm::vec3*>(to) = *static_cast<const glm::vec3*>(from); return;
    case FieldType::Color: *static_cast<glm::vec4*>(to) = *static_cast<const glm::vec4*>(from); return;
    case FieldType::Rotation: *static_cast<glm::quat*>(to) = *static_cast<const glm::quat*>(from); return;
    case FieldType::String:
    case FieldType::AudioClip:
    case FieldType::UIDocument: *static_cast<std::string*>(to) = *static_cast<const std::string*>(from); return;
    case FieldType::Mesh: *static_cast<MeshHandle*>(to) = *static_cast<const MeshHandle*>(from); return;
    case FieldType::Texture: *static_cast<TextureHandle*>(to) = *static_cast<const TextureHandle*>(from); return;
    case FieldType::Model: *static_cast<ModelHandle*>(to) = *static_cast<const ModelHandle*>(from); return;
    case FieldType::Material: *static_cast<MaterialHandle*>(to) = *static_cast<const MaterialHandle*>(from); return;
    case FieldType::MaterialOverrides:
        *static_cast<MaterialOverrides*>(to) = *static_cast<const MaterialOverrides*>(from);
        return;
    case FieldType::Entity: *static_cast<Entity*>(to) = *static_cast<const Entity*>(from); return;
    default: break;
    }
    // A field type added later: through its text, as a file would carry it.
    static const Assets none;
    std::istringstream in(fieldText(f, from, assets));
    std::vector<std::string> ignored;
    readField(in, f, to, assets ? *assets : none, ignored);
}

}   // namespace detail

void writeScene(std::ostream& out, const World& world, const Assets& assets, const SceneFileHooks& hooks) {
    const uint32_t nameId = componentId<Name>();
    const uint32_t instanceId = componentId<PrefabInstance>();
    const std::vector<ComponentInfo>& components = registeredComponents();
    // Prefab parts aren't written: loading makes them again from the prefab.
    auto written = [&](Entity e) { return hooks.exact || !world.has<PrefabPart>(e); };
    // Other entities are referred to by name when it's unique, so links survive blocks
    // being added, removed or reordered; otherwise by block position.
    std::unordered_map<std::string, int> nameCounts;
    world.forEachEntity([&](Entity e) {
        if (const Name* name = world.get<Name>(e); name && !name->value.empty()) ++nameCounts[name->value];
    });
    std::vector<std::string> refs;
    int ordinal = 0;
    world.forEachEntity([&](Entity e) {
        if (e.index >= refs.size()) refs.resize(size_t(e.index) + 1, "none");
        const Name* name = world.get<Name>(e);
        if (name && !name->value.empty() && nameCounts[name->value] == 1) {
            std::ostringstream quoted;
            quoted << std::quoted(name->value);
            refs[e.index] = quoted.str();
        } else if (written(e)) {
            refs[e.index] = std::to_string(ordinal);
        }
        if (written(e)) ++ordinal;
    });

    out << std::setprecision(6);
    if (hooks.prefab) {
        out << "# Shockwave prefab: the first entity is the root, the rest hang under it\n";
        out << "shockwave_prefab 1\n";
    } else {
        out << "# Shockwave scene\n";
        out << "shockwave_scene " << kFormatVersion << "\n";
    }
    if (hooks.writeHeader) hooks.writeHeader(out);

    world.forEachEntity([&](Entity e) {
        if (!written(e)) return;
        // A prefab copy writes only what differs from its prefab.
        const PrefabInstance* instance = world.get<PrefabInstance>(e);
        const PrefabAsset* prefab = instance && !hooks.exact ? instance->source.get() : nullptr;
        out << "\nentity";
        if (const Name* name = world.get<Name>(e)) out << ' ' << std::quoted(name->value);
        out << '\n';
        for (const ComponentInfo& info : components) {
            if (info.id == nameId) continue;   // already on the entity line
            const void* data = info.getConst(world, e);
            if (!data) continue;
            const bool partial = prefab && info.id != instanceId && info.has(prefab->world, prefab->root);
            bool header = false;
            for (const Field& f : info.fields) {
                if (partial && !detail::differsFromPrefab(world, e, info, f, *prefab, &assets)) continue;
                // Unset material fields are left out, so scenes without materials read as before.
                const void* value = static_cast<const char*>(data) + f.offset;
                const bool unset =
                    (f.type == FieldType::Material && !static_cast<const MaterialHandle*>(value)->valid()) ||
                    (f.type == FieldType::MaterialOverrides && static_cast<const MaterialOverrides*>(value)->empty()) ||
                    (info.id == instanceId && f.type == FieldType::String && f.name == "bake_id" &&
                     static_cast<const std::string*>(value)->empty());
                if (!partial && unset) continue;
                if (!header) out << "  " << info.name << '\n';
                header = true;
                out << "    " << f.name;
                writeField(out, f, static_cast<const char*>(data) + f.offset, assets, world, refs);
                out << '\n';
            }
            if (!header && !partial) out << "  " << info.name << '\n';
        }
        if (hooks.exact && world.has<PrefabPart>(e)) out << "  PrefabPart\n";
        out << "end\n";
    });
}

bool saveScene(
    const World& world, const Assets& assets, const std::string& path, std::string& error,
    const SceneFileHooks& hooks) {
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    // Write to a temporary file and rename, so a crash mid-save never eats the scene.
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp);
        if (!out) {
            error = "can't write " + temp;
            return false;
        }
        writeScene(out, world, assets, hooks);
        if (!out) {
            error = "writing " + temp + " failed";
            return false;
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        error = "can't replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

bool loadScene(
    World& world, const Assets& assets, const std::string& path, std::string& error, const SceneFileHooks& hooks,
    std::vector<std::string>* warningsOut) {
    std::ifstream in(path);
    if (!in) {
        error = "can't open " + path;
        return false;
    }
    return readScene(world, assets, in, path, error, hooks, warningsOut);
}

bool readScene(
    World& world, const Assets& assets, std::istream& in, const std::string& path, std::string& error,
    const SceneFileHooks& hooks, std::vector<std::string>* warningsOut) {
    World result = hooks.entities ? hooks.entities->withoutComponents() : World{};
    std::vector<Entity> existing;
    if (hooks.entities) result.forEachEntity([&](Entity e) { existing.push_back(e); });
    std::vector<std::string> warnings;
    // Entity fields can point at blocks further down, so they're filled in at the end.
    struct EntityLink {
        Entity owner;
        const ComponentInfo* component;
        size_t offset;
        int ordinal;   // block position, or -1 with a name
        std::string name;   // empty for a block position or none
        int line;
    };
    std::vector<EntityLink> links;
    std::vector<Entity> created;   // by block position
    // Per block: the fields the file set ("Transform.position"), which a prefab copy keeps
    // over the prefab's values, and the line the block starts on.
    std::vector<std::unordered_set<std::string>> setFields;
    std::vector<int> blockLines;
    int version = 0;
    Entity current = kNullEntity;
    const ComponentInfo* component = nullptr;   // the block we're in
    bool skipping = false;   // inside an unknown component's block
    std::string line;
    int lineNumber = 0;
    auto fail = [&](const std::string& what) {
        error = path + ":" + std::to_string(lineNumber) + ": " + what;
        return false;
    };
    auto warn = [&](const std::string& what) {
        warnings.push_back(path + ":" + std::to_string(lineNumber) + ": " + what);
    };

    while (std::getline(in, line)) {
        ++lineNumber;
        std::istringstream ls(line);
        std::string key;
        if (!(ls >> key) || key[0] == '#') continue;
        // Block keywords start at the left edge; indented lines are fields, so a field may
        // be called "entity" or "end" too.
        const bool indented = line[0] == ' ' || line[0] == '\t';

        if (key == "shockwave_scene" && !indented) {
            if (!(ls >> version) || version < 1 || version > kFormatVersion) return fail("unsupported scene version");
            continue;
        }
        if (key == "shockwave_prefab" && !indented) {   // a prefab: scene format 2 under its own name
            int prefabVersion = 0;
            if (!(ls >> prefabVersion) || prefabVersion != 1) return fail("unsupported prefab version");
            version = kFormatVersion;
            continue;
        }
        if (version == 0) return fail("not a Shockwave scene");

        if (key == "entity" && !indented) {
            if (hooks.entities && created.size() >= existing.size()) return fail("more entities than expected");
            current = hooks.entities ? existing[created.size()] : result.create();
            created.push_back(current);
            setFields.emplace_back();
            blockLines.push_back(lineNumber);
            component = nullptr;
            skipping = false;
            std::string name;
            if (ls >> std::quoted(name)) {
                result.add<Name>(current, {name});
                setFields.back().insert("Name.value");
            }
            if (version == 1) {   // version 1 entities always had a transform and a mesh
                result.add<Transform>(current);
                result.add<MeshRenderer>(current);
            }
            continue;
        }
        if (key == "end" && !indented) {
            current = kNullEntity;
            continue;
        }

        if (!result.alive(current)) {
            if (version == 1 && key == "sky_color") {
                Environment env;
                if (!readFloats(ls, &env.skyColor.r, 3)) return fail("bad sky_color");
                const Entity e = result.create();
                result.add<Name>(e, {"Environment"});
                result.add<Environment>(e, env);
            } else if (hooks.readHeader) {
                hooks.readHeader(key, ls);
            }
            continue;
        }

        if (version == 1) {
            // Version 1: flat entities with a fixed set of properties.
            Transform& t = *result.get<Transform>(current);
            MeshRenderer& m = *result.get<MeshRenderer>(current);
            bool ok = true;
            if (key == "mesh") {
                ok = readField(ls, Field{"mesh", FieldType::Mesh, 0}, &m.mesh, assets, warnings);
            } else if (key == "texture") {
                ok = readField(ls, Field{"texture", FieldType::Texture, 0}, &m.texture, assets, warnings);
            } else if (key == "position") {
                ok = readFloats(ls, &t.position.x, 3);
            } else if (key == "rotation") {
                ok = readField(ls, Field{"rotation", FieldType::Rotation, 0}, &t.rotation, assets, warnings);
            } else if (key == "scale") {
                ok = readFloats(ls, &t.scale.x, 3);
            } else if (key == "tint") {
                ok = readField(ls, Field{"tint", FieldType::Color, 0}, &m.tint, assets, warnings);
            } else if (key == "visible") {
                ok = readField(ls, Field{"visible", FieldType::Bool, 0}, &m.visible, assets, warnings);
            }
            if (!ok) return fail("bad " + key);
            continue;
        }

        // A line with a single word starts a component block; anything else is a field.
        std::istringstream peek(line);
        std::string word;
        peek >> word;
        if (restIsEmpty(peek)) {
            if (key == "PrefabPart") {   // written by exact round trips only
                result.add<PrefabPart>(current);
                component = nullptr;
                skipping = true;
                continue;
            }
            component = findComponent(key);
            skipping = component == nullptr;
            if (component) {
                component->add(result, current);
            } else {
                warn("unknown component '" + key + "' skipped");
            }
            continue;
        }
        if (skipping) continue;
        if (!component) return fail("'" + key + "' outside a component");
        const Field* f = findField(*component, key);
        if (!f) {   // a field this build doesn't have
            warn("unknown field '" + key + "' in " + component->name + " skipped");
            continue;
        }
        auto badValue = [&] { return fail("bad " + component->name + "." + key + ": expected " + valueSyntax(*f)); };
        if (f->type == FieldType::Entity) {
            std::string target;
            if (!(ls >> std::quoted(target))) return badValue();
            int ordinal = -1;
            if (parseInt(target, ordinal) || target == "none") {
                links.push_back({current, component, f->offset, ordinal, {}, lineNumber});
            } else {
                links.push_back({current, component, f->offset, -1, target, lineNumber});
            }
            setFields.back().insert(component->name + "." + key);
            if (!restIsEmpty(ls)) warn("extra values after " + component->name + "." + key + " ignored");
            continue;
        }
        void* data = component->get(result, current);
        const size_t before = warnings.size();
        if (!readField(ls, *f, static_cast<char*>(data) + f->offset, assets, warnings)) return badValue();
        setFields.back().insert(component->name + "." + key);
        for (size_t i = before; i < warnings.size(); ++i) {   // asset lookups don't know the line
            warnings[i] = path + ":" + std::to_string(lineNumber) + ": " + warnings[i];
        }
        if (f->type != FieldType::Color && !restIsEmpty(ls)) {
            warn("extra values after " + component->name + "." + key + " ignored");
        }
        // Limits are the editor's; out-of-range values still load, but are probably a mistake.
        const int count = f->type == FieldType::Float  ? 1
                          : f->type == FieldType::Vec2 ? 2
                          : f->type == FieldType::Vec3 ? 3
                                                       : 0;
        if (f->min != f->max && count > 0) {
            const float* v = reinterpret_cast<const float*>(static_cast<const char*>(data) + f->offset);
            for (int i = 0; i < count; ++i) {
                if (v[i] < f->min || v[i] > f->max) {
                    std::ostringstream range;
                    range << component->name << "." << key << " is outside " << f->min << " to " << f->max;
                    warn(range.str());
                    break;
                }
            }
        }
    }
    if (version == 0) return fail("not a Shockwave scene");

    // Prefab copies: each block with a Prefab gets the prefab's root values under its own,
    // and the prefab's other entities as parts. In an exact round trip the parts were
    // written out, so they only need to know their prefab.
    for (size_t i = 0; i < created.size(); ++i) {
        PrefabInstance* instance = result.get<PrefabInstance>(created[i]);
        if (!instance || instance->path.empty()) continue;
        std::string prefabError;
        auto prefab = prefabs().load(instance->path, assets, &prefabError);
        if (hooks.exact) {
            instance->source = std::move(prefab);
            continue;
        }
        if (!prefab) {
            warnings.push_back(
                path + ":" + std::to_string(blockLines[i]) + ": prefab \"" + instance->path + "\": " + prefabError);
            continue;
        }
        if (result.has<PrefabPart>(created[i])) continue;
        detail::expandPrefab(result, created[i], std::move(prefab), &setFields[i], &assets);
    }

    std::unordered_map<std::string, std::vector<Entity>> byName;
    for (Entity e : created) {
        if (const Name* name = result.get<Name>(e)) byName[name->value].push_back(e);
    }
    // Entities added under a prefab part link to it by name; blocks with the name win.
    std::unordered_map<std::string, std::vector<Entity>> partsByName;
    result.each<const PrefabPart>([&](Entity e, const PrefabPart&) {
        if (const Name* name = result.get<Name>(e); name && !byName.count(name->value))
            partsByName[name->value].push_back(e);
    });
    byName.merge(partsByName);
    for (const EntityLink& link : links) {
        void* data = link.component->get(result, link.owner);
        if (!data) continue;
        Entity target = kNullEntity;
        const std::string at = path + ":" + std::to_string(link.line) + ": ";
        if (!link.name.empty()) {
            const auto found = byName.find(link.name);
            if (found == byName.end()) {
                warnings.push_back(at + "no entity named '" + link.name + "'; left empty");
            } else if (found->second.size() > 1) {
                warnings.push_back(
                    at + std::to_string(found->second.size()) + " entities are named '" + link.name +
                    "'; give the target a unique name. Left empty");
            } else {
                target = found->second.front();
            }
        } else if (link.ordinal >= 0 && size_t(link.ordinal) < created.size()) {
            target = created[size_t(link.ordinal)];
        } else if (link.ordinal >= 0) {
            warnings.push_back(
                path + ":" + std::to_string(link.line) + ": entity " + std::to_string(link.ordinal) +
                " doesn't exist; left empty");
        }
        *reinterpret_cast<Entity*>(static_cast<char*>(data) + link.offset) = target;
    }

    world = std::move(result);
    if (warningsOut) *warningsOut = std::move(warnings);
    return true;
}

void writeSceneSchema(std::ostream& out, const Assets& assets) {
    out << std::setprecision(6);
    out << "# Shockwave scene schema\n\n"
        << "Generated by `player --schema` from the engine's component registry and this project's\n"
        << "assets. Regenerate it after adding components or assets; don't edit it by hand.\n\n"
        << "## File layout\n\n"
        << "```text\n"
        << "# Comments start with #.\n"
        << "shockwave_scene " << kFormatVersion << "\n\n"
        << "entity \"Stack\"\n"
        << "  Transform\n"
        << "    position 0 0 0\n"
        << "end\n\n"
        << "entity \"Upper crate\"\n"
        << "  Transform\n"
        << "    position 0 1 0\n"
        << "  Parent\n"
        << "    entity \"Stack\"\n"
        << "  MeshRenderer\n"
        << "    mesh cube\n"
        << "    texture panel\n"
        << "end\n"
        << "```\n\n"
        << "- `entity \"Name\"` and `end` start at the left edge. Component names are indented two\n"
        << "  spaces, fields four. Names should be unique: links to other entities use them.\n"
        << "- A component line is the component name alone. Each field line is the field name and\n"
        << "  its value. Leave out fields to keep their defaults, and components to not have them.\n"
        << "- Values with spaces are quoted. Numbers use `.` for decimals.\n"
        << "- +Y is up, -Z is forward, units are meters. A Transform under a Parent is relative to it.\n"
        << "- Every playable scene needs one entity with a `Camera` (primary 1): the game looks\n"
        << "  through it.\n"
        << "- Lighting: a `Light` shines along its Transform's -Z (rotation -90 0 0 points straight\n"
        << "  down); position only matters for Point and Spot. Colors are linear. Intensity 1 lights\n"
        << "  a white surface facing the light to full white before exposure; a sun is usually 2 to\n"
        << "  4, a lamp 3 to 20. Point and Spot fall off to nothing at `range`. With no Light at all\n"
        << "  the renderer adds a default sun. The first `Environment` sets the light from all\n"
        << "  around (an `environment_map` panorama, ideally a .hdr file, or else the sky and\n"
        << "  ground colors), exposure (in stops) and tonemapper. See docs/Lighting.md.\n"
        << "- See a scene as the game renders it with `player --screenshot out.png --scene path`\n"
        << "  (`--view normals|lighting|unlit` to isolate a problem).\n"
        << "- Check a file with `player --check path/to/scene.swscene` (no path: every scene in the\n"
        << "  project). It prints one `file:line: problem` per issue and exits non-zero on any.\n\n"
        << "## Prefabs\n\n"
        << "A prefab is a reusable group of entities in a `.swprefab` file inside Content. It's\n"
        << "written like a scene with `shockwave_prefab 1` as its header line. Its first entity is\n"
        << "the root. Entities without a `Parent` hang under the root, and their Transforms are\n"
        << "relative to it. Prefabs can contain copies of other prefabs, but not of themselves.\n\n"
        << "To place a copy, give an entity a `Prefab` component with the file's path inside\n"
        << "Content. Then list only what this copy changes on the prefab's root, usually its\n"
        << "Transform. Every other value comes from the prefab:\n\n"
        << "```text\n"
        << "entity \"Lamp by the door\"\n"
        << "  Prefab\n"
        << "    path \"Prefabs/Street lamp.swprefab\"\n"
        << "  Transform\n"
        << "    position 4 0 -2\n"
        << "end\n"
        << "```\n\n"
        << "- The prefab's other entities are created under the copy when the scene loads. They\n"
        << "  aren't written in the scene. To change them, edit the prefab.\n"
        << "- New entities can go under a copy (`Parent` names the copy, or a part of it by name).\n"
        << "- `player --check` also checks every `.swprefab` in Content.\n\n"
        << "## Components\n";

    // Defaults come from default-constructed components, written exactly as a file would.
    World scratch;
    const Entity probe = scratch.create();
    const std::vector<std::string> noRefs;
    std::string category = "\x01";
    std::vector<const ComponentInfo*> sorted;
    for (const ComponentInfo& info : registeredComponents()) {
        if (info.id != componentId<Name>()) sorted.push_back(&info);
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const ComponentInfo* a, const ComponentInfo* b) {
        return a->category < b->category;
    });
    for (const ComponentInfo* info : sorted) {
        if (info->category != category) {
            category = info->category;
            out << "\n### " << (category.empty() ? "Other" : category) << "\n";
        }
        info->add(scratch, probe);
        const char* data = static_cast<const char*>(info->getConst(scratch, probe));
        out << "\n#### " << info->name << "\n\n";
        if (info->fields.empty()) {
            out << "No fields; the component's presence is what matters.\n";
        } else {
            out << "| Field | Value | Default |\n|---|---|---|\n";
            for (const Field& f : info->fields) {
                std::ostringstream value;
                value << std::setprecision(6);
                writeField(value, f, data + f.offset, assets, scratch, noRefs);
                std::string syntax = valueSyntax(f);
                if (f.min != f.max) {
                    std::ostringstream range;
                    range << std::setprecision(6) << "; range " << f.min << " to " << f.max;
                    syntax += range.str();
                }
                const std::string written = value.str();   // " value"; empty for an empty list
                out << "| `" << f.name << "` | " << syntax << " | "
                    << (written.empty() ? std::string("none") : "`" + written.substr(1) + "`") << " |\n";
            }
        }
        info->remove(scratch, probe);
    }

    auto list = [&](const char* title, const std::vector<std::string>& names) {
        out << "\n### " << title << "\n\n";
        if (names.empty()) {
            out << "None.\n";
            return;
        }
        for (const std::string& name : names) out << "- `" << name << "`\n";
    };
    out << "\n## Assets\n\nNames a scene can use, built-in and from this project's asset folder.\n";
    std::vector<std::string> names;
    for (const auto& [name, handle] : assets.meshes()) names.push_back(name);
    list("Meshes (MeshRenderer.mesh)", names);
    names.clear();
    for (const auto& [name, handle] : assets.textures()) names.push_back(name);
    list("Textures (MeshRenderer.texture)", names);
    names.clear();
    for (const auto& [name, handle] : assets.models()) names.push_back(name);
    list("Models (ModelRenderer.model, Collider convex/triangle shapes)", names);
    names.clear();
    for (const auto& [name, handle] : assets.materials()) names.push_back(name);
    list("Materials (MeshRenderer.material, ModelRenderer.materials)", names);
    list("Audio clips (AudioSource.clip)", assets.audioClips());
    names.clear();
    std::error_code ec;
    const std::filesystem::path content = prefabs().contentFolder();
    if (!content.empty()) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(content, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".swprefab")
                names.push_back(entry.path().lexically_relative(content).generic_string());
        }
    }
    std::sort(names.begin(), names.end());
    list("Prefabs (Prefab.path)", names);
}

}   // namespace shockwave
