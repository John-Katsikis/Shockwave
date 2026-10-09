#include <shockwave/Components.h>
#include <shockwave/SceneFile.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace shockwave {

namespace {

constexpr int kFormatVersion = 2;

// Six significant digits, and no "-0" in files.
struct Num {
    float v;
};
std::ostream& operator<<(std::ostream& out, Num n) {
    return out << (n.v == 0.0f ? 0.0f : n.v);
}

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

// entityOrdinals maps an entity index to the position of its block in the file.
void writeField(std::ostream& out, const Field& f, const void* data, const Assets& assets, const World& world,
                const std::vector<int>& entityOrdinals) {
    switch (f.type) {
    case FieldType::Bool: out << ' ' << (*static_cast<const bool*>(data) ? 1 : 0); break;
    case FieldType::Int: out << ' ' << *static_cast<const int*>(data); break;
    case FieldType::Float: writeFloats(out, static_cast<const float*>(data), 1); break;
    case FieldType::Vec3: writeFloats(out, static_cast<const float*>(data), 3); break;
    case FieldType::Color: writeFloats(out, static_cast<const float*>(data), 4); break;
    case FieldType::Rotation: {
        const glm::vec3 euler = eulerDegrees(*static_cast<const glm::quat*>(data));
        writeFloats(out, &euler.x, 3);
        break;
    }
    case FieldType::String: out << ' ' << std::quoted(*static_cast<const std::string*>(data)); break;
    case FieldType::Mesh: {
        const std::string name = assets.meshName(*static_cast<const MeshHandle*>(data));
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
        out << ' ' << (world.alive(e) ? entityOrdinals[e.index] : -1);
        break;
    }
    }
}

// Returns false if the value doesn't parse; unknown asset names load as defaults with a warning.
bool readField(std::istream& in, const Field& f, void* data, const Assets& assets, std::vector<std::string>& warnings) {
    switch (f.type) {
    case FieldType::Bool: {
        int v = 0;
        if (!(in >> v)) return false;
        *static_cast<bool*>(data) = v != 0;
        return true;
    }
    case FieldType::Int: return static_cast<bool>(in >> *static_cast<int*>(data));
    case FieldType::Float: return readFloats(in, static_cast<float*>(data), 1);
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
    case FieldType::String: return static_cast<bool>(in >> std::quoted(*static_cast<std::string*>(data)));
    case FieldType::Mesh: {
        std::string name;
        if (!(in >> std::quoted(name))) return false;
        const MeshHandle mesh = assets.findMesh(name);
        if (!mesh.valid() && name != "none") warnings.push_back("unknown mesh '" + name + "'");
        *static_cast<MeshHandle*>(data) = mesh;
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

} // namespace

bool saveScene(const World& world, const Assets& assets, const std::string& path, std::string& error,
               const SceneFileHooks& hooks) {
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    const uint32_t nameId = componentId<Name>();
    const std::vector<ComponentInfo>& components = registeredComponents();
    std::vector<int> ordinals;
    int ordinal = 0;
    world.forEachEntity([&](Entity e) {
        if (e.index >= ordinals.size()) ordinals.resize(size_t(e.index) + 1, -1);
        ordinals[e.index] = ordinal++;
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
                    writeField(out, f, static_cast<const char*>(data) + f.offset, assets, world, ordinals);
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

bool loadScene(World& world, const Assets& assets, const std::string& path, std::string& error,
               const SceneFileHooks& hooks, std::vector<std::string>* warningsOut) {
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
        int ordinal;
        int line;
    };
    std::vector<EntityLink> links;
    std::vector<Entity> created;   // by block position
    int version = 0;
    Entity current = kNullEntity;
    const ComponentInfo* component = nullptr;   // the block we're in
    bool skipping = false;                      // inside an unknown component's block
    std::string line;
    int lineNumber = 0;
    auto fail = [&](const std::string& what) {
        error = path + ":" + std::to_string(lineNumber) + ": " + what;
        return false;
    };
    auto warn = [&](const std::string& what) { warnings.push_back(path + ":" + std::to_string(lineNumber) + ": " + what); };

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
        if (!f) continue;   // a field this build doesn't have
        if (f->type == FieldType::Entity) {
            int ordinal = -1;
            if (!(ls >> ordinal)) return fail("bad " + component->name + "." + key);
            links.push_back({current, component, f->offset, ordinal, lineNumber});
            continue;
        }
        void* data = component->get(result, current);
        if (!readField(ls, *f, static_cast<char*>(data) + f->offset, assets, warnings)) {
            return fail("bad " + component->name + "." + key);
        }
    }
    if (version == 0) return fail("not a Shockwave scene");

    for (const EntityLink& link : links) {
        void* data = link.component->get(result, link.owner);
        if (!data) continue;
        Entity target = kNullEntity;
        if (link.ordinal >= 0 && size_t(link.ordinal) < created.size()) {
            target = created[size_t(link.ordinal)];
        } else if (link.ordinal >= 0) {
            warnings.push_back(path + ":" + std::to_string(link.line) + ": entity " + std::to_string(link.ordinal) +
                               " doesn't exist; left empty");
        }
        *reinterpret_cast<Entity*>(static_cast<char*>(data) + link.offset) = target;
    }

    world = std::move(result);
    if (warningsOut) *warningsOut = std::move(warnings);
    return true;
}

} // namespace shockwave
