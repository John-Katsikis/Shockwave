#include <Engine/SceneFile.h>

#include <Engine/Components.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>

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

bool saveScene(
    const World& world, const Assets& assets, const std::string& path, std::string& error,
    const SceneFileHooks& hooks) {
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    const uint32_t nameId = componentId<Name>();
    const std::vector<ComponentInfo>& components = registeredComponents();
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
        } else {
            refs[e.index] = std::to_string(ordinal);
        }
        ++ordinal;
    });

    // Write to a temporary file and rename, so a crash mid-save never eats the scene.
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp);
        if (!out) {
            error = "can't write " + temp;
            return false;
        }
        out << std::setprecision(6);
        out << "# Shockwave scene\n";
        out << "shockwave_scene " << kFormatVersion << "\n";
        if (hooks.writeHeader) hooks.writeHeader(out);

        world.forEachEntity([&](Entity e) {
            out << "\nentity";
            if (const Name* name = world.get<Name>(e)) out << ' ' << std::quoted(name->value);
            out << '\n';
            for (const ComponentInfo& info : components) {
                if (info.id == nameId) continue;   // already on the entity line
                const void* data = info.getConst(world, e);
                if (!data) continue;
                out << "  " << info.name << '\n';
                for (const Field& f : info.fields) {
                    out << "    " << f.name;
                    writeField(out, f, static_cast<const char*>(data) + f.offset, assets, world, refs);
                    out << '\n';
                }
            }
            out << "end\n";
        });
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

    World result;
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
        if (version == 0) return fail("not a Shockwave scene");

        if (key == "entity" && !indented) {
            current = result.create();
            created.push_back(current);
            component = nullptr;
            skipping = false;
            std::string name;
            if (ls >> std::quoted(name)) result.add<Name>(current, {name});
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
            if (!restIsEmpty(ls)) warn("extra values after " + component->name + "." + key + " ignored");
            continue;
        }
        void* data = component->get(result, current);
        const size_t before = warnings.size();
        if (!readField(ls, *f, static_cast<char*>(data) + f->offset, assets, warnings)) return badValue();
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

    std::unordered_map<std::string, std::vector<Entity>> byName;
    for (Entity e : created) {
        if (const Name* name = result.get<Name>(e)) byName[name->value].push_back(e);
    }
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
        << "  the renderer adds a default sun. The first `Environment` sets the sky and ground\n"
        << "  ambient, exposure (in stops) and tonemapper.\n"
        << "- See a scene as the game renders it with `player --screenshot out.png --scene path`\n"
        << "  (`--view normals|lighting|unlit` to isolate a problem).\n"
        << "- Check a file with `player --check path/to/scene.swscene` (no path: every scene in the\n"
        << "  project). It prints one `file:line: problem` per issue and exits non-zero on any.\n\n"
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
                out << "| `" << f.name << "` | " << syntax << " | `" << value.str().substr(1) << "` |\n";
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
    list("Audio clips (AudioSource.clip)", assets.audioClips());
}

}   // namespace shockwave
