#pragma once

// What scenes and prefabs refer to, for the asset dependency graph (S04,
// docs/AssetResidency.md, Assets/AssetGraph.h).
//
// Fields of the asset types (Mesh, Texture, Model, Material, MaterialOverrides, AudioClip,
// UIDocument) are references by their type. A String field that holds a path inside
// Content says so once, where its component is registered:
//
//   registerAssetPathField("ParticleEffect", "effect", AssetKind::Effect);
//
// Game modules can register their own the same way.

#include <Assets/AssetGraph.h>
#include <Engine/Components.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

// A String field holding a Content path of this kind. Not required: fine when the file
// isn't there yet (a lighting bake not made). Startup, main thread.
struct AssetPathField {
    AssetKind kind = AssetKind::Other;
    bool required = true;
};
void registerAssetPathField(std::string component, std::string field, AssetKind kind, bool required = true);
std::optional<AssetPathField> assetPathField(std::string_view component, std::string_view field);

struct SceneAssetReference {
    std::string entity;   // its name, or "#n" for the nth block
    std::string component, field;
    FieldType type = FieldType::String;
    AssetKind kind = AssetKind::Other;
    std::string value;   // as written: a name or a Content path
    int line = 0;
    bool required = true;
};

// Every asset a scene or prefab file names, read with the scene grammar and the component
// registry, without building a world. Prefabs it uses are references too (Prefab.path),
// not expanded. False with error when the file isn't a scene.
bool sceneAssetReferences(
    std::string_view text, const std::string& path, std::vector<SceneAssetReference>& out, std::string& error);

// The scene, prefab and post-process providers, and the Engine's own asset path fields.
// Called by the Engine's component registration; safe to repeat.
void registerEngineAssetProviders();

}   // namespace shockwave
