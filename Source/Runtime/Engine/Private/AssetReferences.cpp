// Scene and prefab references for the asset graph: see Engine/AssetReferences.h.
#include <Engine/AssetReferences.h>

#include <Engine/PostProcess.h>

#include <iomanip>
#include <map>
#include <sstream>

namespace shockwave {

namespace {

std::map<std::pair<std::string, std::string>, AssetPathField, std::less<>>& pathFields() {
    static std::map<std::pair<std::string, std::string>, AssetPathField, std::less<>> fields;
    return fields;
}

bool onlySpaces(std::istream& in) {
    std::string rest;
    return !(in >> rest);
}

AssetKind kindOfType(FieldType type) {
    switch (type) {
    case FieldType::Mesh: return AssetKind::Other;   // a built-in name, or a file of some kind
    case FieldType::Texture: return AssetKind::Texture;
    case FieldType::Model: return AssetKind::Model;
    case FieldType::Material:
    case FieldType::MaterialOverrides: return AssetKind::Material;
    case FieldType::AudioClip: return AssetKind::Audio;
    case FieldType::UIDocument: return AssetKind::UIDocument;
    default: return AssetKind::Other;
    }
}

}   // namespace

void registerAssetPathField(std::string component, std::string field, AssetKind kind, bool required) {
    pathFields()[{std::move(component), std::move(field)}] = {kind, required};
}

std::optional<AssetPathField> assetPathField(std::string_view component, std::string_view field) {
    const auto it = pathFields().find(std::pair<std::string, std::string>(component, field));
    if (it == pathFields().end()) return std::nullopt;
    return it->second;
}

bool sceneAssetReferences(
    std::string_view text, const std::string& path, std::vector<SceneAssetReference>& out, std::string& error) {
    std::istringstream in{std::string(text)};
    std::string line, entity;
    int lineNumber = 0, version = 0, blocks = 0;
    const ComponentInfo* component = nullptr;
    bool skipping = false;
    auto add = [&](std::string comp, std::string field, FieldType type, AssetKind kind, std::string value,
                   bool required = true) {
        if (value.empty()) return;
        out.push_back({entity, std::move(comp), std::move(field), type, kind, std::move(value), lineNumber, required});
    };
    while (std::getline(in, line)) {
        ++lineNumber;
        std::istringstream ls(line);
        std::string key;
        if (!(ls >> key) || key[0] == '#') continue;
        const bool indented = line[0] == ' ' || line[0] == '\t';
        if (!indented && key == "shockwave_scene") {
            ls >> version;
            continue;
        }
        if (!indented && key == "shockwave_prefab") {
            version = 2;
            continue;
        }
        if (version == 0) {
            error = path + ":" + std::to_string(lineNumber) + ": not a Shockwave scene";
            return false;
        }
        if (!indented && key == "entity") {
            std::string name;
            entity = ls >> std::quoted(name) ? name : "#" + std::to_string(blocks);
            ++blocks;
            component = nullptr;
            skipping = false;
            continue;
        }
        if (!indented && key == "end") {
            entity.clear();
            continue;
        }
        if (entity.empty()) continue;   // header lines
        if (version == 1) {   // flat entities: a mesh and a texture
            std::string value;
            if (key == "mesh" && ls >> std::quoted(value))
                add("MeshRenderer", "mesh", FieldType::Mesh, AssetKind::Other, value);
            if (key == "texture" && ls >> std::quoted(value))
                add("MeshRenderer", "texture", FieldType::Texture, AssetKind::Texture, value);
            continue;
        }
        std::istringstream peek(line);
        std::string word;
        peek >> word;
        if (onlySpaces(peek)) {   // a component block
            component = key == "PrefabPart" ? nullptr : findComponent(key);
            skipping = component == nullptr;
            continue;
        }
        if (skipping || !component) continue;
        const Field* field = nullptr;
        for (const Field& f : component->fields)
            if (f.name == key) field = &f;
        if (!field) continue;
        std::string value;
        switch (field->type) {
        case FieldType::Mesh:
        case FieldType::Texture:
        case FieldType::Model:
        case FieldType::Material:
        case FieldType::AudioClip:
        case FieldType::UIDocument:
            if (ls >> std::quoted(value)) add(component->name, key, field->type, kindOfType(field->type), value);
            break;
        case FieldType::MaterialOverrides: {
            std::string part;
            while (ls >> std::quoted(part) && ls >> std::quoted(value))
                add(component->name, key, field->type, AssetKind::Material, value);
            break;
        }
        case FieldType::String:
            if (const auto path = assetPathField(component->name, key); path && ls >> std::quoted(value))
                add(component->name, key, field->type, path->kind, value, path->required);
            break;
        default: break;
        }
    }
    if (version == 0) {
        error = path + ": not a Shockwave scene";
        return false;
    }
    return true;
}

void registerEngineAssetProviders() {
    static bool done = false;
    if (done) return;
    done = true;
    registerAssetPathField("Prefab", "path", AssetKind::Prefab);
    // A bake not made yet is a warning of its own (player --check), not a broken reference.
    registerAssetPathField("BakedLighting", "asset", AssetKind::Lighting, false);
    registerAssetPathField("BakedLighting", "reflections", AssetKind::Reflections, false);
    registerAssetPathField("PostProcessVolume", "profile", AssetKind::PostProcess);
    // Scenes and prefabs: what their fields name. A mesh, texture or model without an
    // extension is a built-in name.
    registerAssetProvider("scene", {".swscene", ".swprefab"}, [](AssetReader& r, std::string& error) {
        std::vector<SceneAssetReference> refs;
        const bool ok = sceneAssetReferences(r.text(), r.id(), refs, error);
        for (const SceneAssetReference& ref : refs) {
            const std::string via = "entity '" + ref.entity + "': " + ref.component + "." + ref.field;
            switch (ref.type) {
            case FieldType::Mesh: r.named(ref.value, AssetKind::Builtin, via, ref.line); break;
            case FieldType::Texture:
                if (ref.value != "white") r.named(ref.value, AssetKind::Texture, via, ref.line);
                break;
            case FieldType::Model: r.named(ref.value, AssetKind::Model, via, ref.line); break;
            default: r.content(ref.value, via, ref.line, ref.kind, ref.required); break;
            }
        }
        return ok;
    });
    // A post-process profile: its parent and its colour grading table.
    registerAssetProvider("postprocess", {".swpostprocess"}, [](AssetReader& r, std::string& error) {
        PostProcessFile file;
        if (!parsePostProcess(r.text(), file, error)) return false;
        if (!file.parent.empty()) r.content(file.parent, "parent", 0, AssetKind::PostProcess);
        if (!file.values.lut.empty()) r.content(file.values.lut, "lut", 0, AssetKind::Lut);
        return true;
    });
}

}   // namespace shockwave
