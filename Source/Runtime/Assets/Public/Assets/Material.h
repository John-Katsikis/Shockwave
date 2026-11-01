#pragma once

// Materials as assets (A02, docs/Graphics/02-MaterialsAndDecals.md, docs/MaterialSchema.md).
//
// A .swmaterial file is a small text asset: a parent to start from (another .swmaterial, or
// a material imported with a model, named by the model and the material's name), and the
// values it changes. Everything it doesn't set comes from the parent, or from the defaults
// below. It never copies texture pixels, only names the textures to use:
//
//     swmaterial 1
//     parent "models/crate.glb" "Rusty metal"
//     roughness 0.32
//     texture detail_normal "textures/scratches_normal.png"
//     uv_transform detail_normal 0 0 8 8 0
//
// Parsing is strict: an unknown key, slot or version, a value out of range, a record given
// twice, or anything left over on a line fails with its line number. Resolving follows the
// parents (at most kMaxMaterialDepth files, no cycles) and gives the complete, immutable
// description a renderer needs (MaterialDesc). Saving writes the canonical form: records in
// schema order, comments dropped.

#include <Assets/Model.h>
#include <Renderer/Shading.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

struct MaterialHandle {
    uint32_t id = UINT32_MAX;
    bool valid() const { return id != UINT32_MAX; }
    bool operator==(const MaterialHandle&) const = default;
};

// Shading families (A07-A09, docs/Shading.md), in ShadingFamily's order: standard (glTF
// metallic-roughness, optionally with a clearcoat), skin, hair cards and transmission.
enum class MaterialFamily : int { Standard, Skin, HairCard, Transmission };
enum class MaterialAlpha : int { Opaque, Mask, Blend };
// Which channel of a mask texture is read.
enum class MaskChannel : int { R, G, B, A };
// Where the second layer's mask comes from: a channel of the layer_mask texture, or a
// colour channel of the vertices (whose colours then don't tint the surface; their alpha
// still multiplies alpha as usual, so there's no vertex alpha mask).
enum class LayerMaskSource : int { TextureR, TextureG, TextureB, TextureA, VertexR, VertexG, VertexB };

// Every value a material has, after inheritance. Colours are linear scene RGB; textures in
// colour slots are decoded from sRGB, the rest are read as linear data.
struct MaterialParams {
    int family = int(MaterialFamily::Standard);
    glm::vec4 baseColor{1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;   // perceptual; 0 is kept as written, the shader clamps it
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    glm::vec3 emissive{0.0f};
    int alphaMode = int(MaterialAlpha::Opaque);
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    // Detail layer (2.6).
    float detailStrength = 1.0f;
    float detailNormalScale = 1.0f;
    float detailRoughness = 0.5f;
    float detailRoughnessStrength = 0.0f;
    int detailMaskChannel = int(MaskChannel::R);
    glm::vec2 detailFade{0.0f};   // start and end, metres from the camera; end 0: no fade
    // Second layer (2.7).
    int layerMask = int(LayerMaskSource::TextureR);
    glm::vec4 layerBaseColor{1.0f};
    float layerMetallic = 0.0f;
    float layerRoughness = 0.5f;
    float layerNormalScale = 1.0f;
    // Clearcoat (A07a), family standard: a dielectric coat over the base.
    float clearcoat = 0.0f;
    float clearcoatRoughness = 0.0f;
    float clearcoatNormalScale = 1.0f;
    // Transmission (A07b), family transmission.
    float transmission = 1.0f;
    float ior = 1.5f;
    float thickness = 0.01f;   // metres, object space
    int transmissionMode = 0;   // 0 thin, 1 solid
    glm::vec3 attenuationColor{1.0f};
    float attenuationDistance = 0.0f;   // metres; 0: no absorption
    // Skin (A08), family skin.
    glm::vec3 skinRadius{3.7f, 1.4f, 1.1f};   // millimetres per channel
    float skinStrength = 1.0f;
    float skinMaxPixels = 24.0f;
    // Hair cards (A09), family hair_card.
    float hairAnisotropy = 0.8f;
    int hairDirection = 1;   // 0 u, 1 v: root to tip
    bool operator==(const MaterialParams&) const = default;
};

// ---- The schema: one descriptor per value, which the parser, writer, editor and
// player --material-schema all read ----
enum class MaterialValueType { Float, Vec2, Color3, Color4, Bool, Enum };
struct MaterialParamInfo {
    const char* key;   // as written in the file
    MaterialValueType type;
    size_t offset;   // in MaterialParams
    float min, max;   // per component; min == max: any finite value
    std::vector<const char*> options;   // Enum: the names, in value order
    const char* group;   // "surface", "alpha", "detail", "layer", "clearcoat", "transmission", "skin", "hair"
    const char* description;
};
const std::vector<MaterialParamInfo>& materialParams();
const MaterialParamInfo* findMaterialParam(std::string_view key);
inline constexpr float kMaxEmissive = 1000.0f;   // the engine's upper bound for emissive components

// What each texture slot holds and how it's decoded.
struct MaterialSlotInfo {
    MaterialSlot slot;
    bool srgb;   // colour (decoded from sRGB) or data (linear)
    bool normalMap;   // tangent-space normals
    const char* group;
    const char* description;
};
const MaterialSlotInfo& materialSlotInfo(MaterialSlot slot);
bool parseMaterialSlot(std::string_view name, MaterialSlot& slot);

// ---- A .swmaterial file: its parent and what it changes ----
struct MaterialSlotOverride {
    bool hasTexture = false;
    std::string texture;   // asset name; empty with hasTexture: clear_texture
    bool hasUvSet = false;
    uint32_t uvSet = 0;
    bool hasTransform = false;
    UvTransform transform;
    bool hasSampler = false;
    SamplerDesc sampler;
    bool empty() const { return !hasTexture && !hasUvSet && !hasTransform && !hasSampler; }
    bool operator==(const MaterialSlotOverride&) const = default;
};
struct MaterialFile {
    std::string parent;   // a .swmaterial or a model, by asset name; empty: the schema's defaults
    std::string parentMaterial;   // with a model parent: the source material's name in it
    MaterialParams values;   // only those in set mean anything
    uint64_t set = 0;   // bit i: materialParams()[i] is overridden
    std::array<MaterialSlotOverride, kAllMaterialSlots> slots{};

    bool isSet(std::string_view key) const;
    void setValue(std::string_view key, bool on);   // marks or clears an override
};
bool isMaterialFile(const std::filesystem::path& path);   // .swmaterial
// Errors are "line N: what's wrong".
bool parseMaterial(std::string_view text, MaterialFile& file, std::string& error);
std::string writeMaterial(const MaterialFile& file);
bool loadMaterialFile(const std::filesystem::path& path, MaterialFile& file, std::string& error);
// Through a temporary file, so a reader never sees half of it.
bool saveMaterialFile(const std::filesystem::path& path, const MaterialFile& file, std::string& error);
// The format reference (docs/MaterialSchema.md, player --material-schema), from the
// descriptors above.
std::string materialSchemaMarkdown();

// ---- Resolved: what a material is, ready to draw ----
// A texture a slot reads: a texture asset, or one of a model's own (an imported parent's).
struct MaterialTexture {
    std::string asset;   // empty: none
    int modelTexture = -1;   // >= 0: index into that model's textures
    bool empty() const { return asset.empty(); }
    bool operator==(const MaterialTexture&) const = default;
};
struct ResolvedSlot {
    MaterialTexture texture;
    ModelTextureBinding binding;
    bool operator==(const ResolvedSlot&) const = default;
};
// Canonical features: only what will actually be evaluated (a detail layer with no detail
// textures, or a strength of 0, is not one). A small bounded set; scalars never add one.
enum MaterialFeature : uint32_t {
    kMaterialAlphaMask = 1u << 0,
    kMaterialAlphaBlend = 1u << 1,
    kMaterialNormalMap = 1u << 2,
    kMaterialDetail = 1u << 3,
    kMaterialDetailNormal = 1u << 4,
    kMaterialLayer = 1u << 5,
    kMaterialDoubleSided = 1u << 6,
    kMaterialClearcoat = 1u << 7,   // standard with a coat above 0
    kMaterialTransmission = 1u << 8,   // transmission with transmission above 0 (or its texture)
    kMaterialSkin = 1u << 9,
    kMaterialHairCard = 1u << 10,
};
inline constexpr uint32_t kMaterialFeatureCount = 11;
const char* materialFeatureName(uint32_t feature);   // "alpha_mask", ...
struct MaterialDesc {
    MaterialParams params;
    std::array<ResolvedSlot, kAllMaterialSlots> slots{};
    // Where it came from: the material itself first, then each parent; an imported material
    // as "models/crate.glb#Rusty metal".
    std::vector<std::string> chain;
    std::vector<std::string> dependencies;   // asset names it was resolved from
    std::vector<std::string> warnings;   // fields that do nothing, and why
    uint32_t features = 0;
    bool operator==(const MaterialDesc&) const = default;
};
uint32_t materialFeatures(const MaterialDesc& desc);
// Which groups of values the family reads ("clearcoat" for standard, "transmission", ...):
// the editor hides the others, --check warns about values set for another family.
bool materialFamilyUses(int family, std::string_view group);
// What the material draws as with the given shading settings, for --material-info and the
// editor: "standard", "standard with clearcoat", "simplified glass", "skin without
// diffusion", ...
std::string materialEffectiveShading(const MaterialDesc& desc, const ShadingSettings& settings);
// The passes and memory the family needs ("transmission pass, scene copy with mips"), or
// empty for none beyond the scene's.
std::string materialShadingCost(const MaterialDesc& desc, const ShadingSettings& settings);
// What material_quality low leaves out of this material ("detail_color", ...); empty when
// nothing is.
std::vector<std::string> materialLowQualityOmissions(const MaterialDesc& desc);
// An imported material, as a parent sees it.
MaterialDesc importedMaterial(const ModelData& model, std::string_view modelName, uint32_t index);
// The model's material called name. Materials are found by name, never by position, so
// reordering them in the source file keeps every reference; an unnamed or ambiguous one
// can't be referred to. -1 with error if not found.
int findSourceMaterial(const ModelData& model, std::string_view name, std::string& error);

inline constexpr size_t kMaxMaterialDepth = 16;   // .swmaterial files in one chain
// How resolve finds what a material refers to. Any can be left empty: file and model
// then fail, texture accepts every name.
struct MaterialLookup {
    std::function<bool(std::string_view name, MaterialFile& file, std::string& error)> file;
    std::function<const ModelData*(std::string_view name, std::string& error)> model;
    std::function<bool(std::string_view name)> texture;
};
// Reads the material files from a folder (Content), models through loadModel. For tools.
MaterialLookup folderMaterialLookup(const std::filesystem::path& root);
enum class MaterialProblem { None, File, Reference };
// File: the material or a parent doesn't parse (or is a cycle, or too deep); Reference: a
// parent, model, source material or texture it names isn't there.
MaterialProblem
resolveMaterial(std::string_view name, const MaterialLookup& lookup, MaterialDesc& desc, std::string& error);

// What the renderer draws a resolved material with: textures and samplers already made,
// per slot (MaterialSlot order). layers points into the result's own storage, so keep it
// where it is once made (the asset holds it).
struct MaterialAsset {
    MaterialDesc desc;
    TextureHandle baseColorTexture;
    glm::vec4 baseColor{1.0f};
    DrawMaterial draw;
    MaterialLayers layers;
    MaterialShading shading;   // DrawMaterial::shading points here (A07-A09)
    std::array<TextureHandle, kAllMaterialSlots> textures{};   // per slot, what draw uses
    std::array<SamplerHandle, kAllMaterialSlots> samplers{};
    bool fallback = false;   // couldn't be resolved: drawn magenta, see error
    std::string error;
    uint64_t generation = 0;   // counts up each time the material is resolved again
};
void buildDrawMaterial(
    MaterialAsset& asset, const std::array<TextureHandle, kAllMaterialSlots>& textures,
    const std::array<SamplerHandle, kAllMaterialSlots>& samplers);
// What a model's own material draws with: ModelAsset::drawMaterials[material], or worked
// out the same way for a model put together without them (tests, tools).
DrawMaterial modelDrawMaterial(const ModelAsset& model, uint32_t material);
// The visible stand-in for a material that couldn't be resolved: flat magenta.
MaterialAsset fallbackMaterial(std::string error);

// ---- Shading math, as the shaders do it (tests check these against the GLSL) ----
// Reoriented normal mapping (Barre-Brisebois and Hill 2012): detail d on base b, both unit
// tangent-space normals. Neutral detail (0 0 1) returns b, neutral base returns d. Where b
// points almost straight back (b.z near -1) the formula breaks down; it then turns d half a
// turn about x instead.
glm::vec3 composeNormals(glm::vec3 base, glm::vec3 detail);
// Specular anti-aliasing for a filtered normal map (Toksvig 2005): a mip's mean normal
// shortens where the normals under it disagree, and the highlight should widen to match.
// alpha is GGX's (perceptual roughness squared); length the decoded normal's length.
float toksvigAlpha(float alpha, float length);

}   // namespace shockwave
