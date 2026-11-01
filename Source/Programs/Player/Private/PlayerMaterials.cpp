// --material-info, --material-schema and --texture-info: what the importer makes of an
// asset's materials and textures, through the same code gameplay loads them with, without a
// GPU (docs/GraphicsVerification.md, G02, A01 and A02); and --check's material rules.

#include "Player.h"

#include <Assets/AssetFolder.h>
#include <Assets/Image.h>
#include <Assets/ImportSettings.h>
#include <Assets/Material.h>
#include <Assets/Model.h>
#include <Assets/TextureCompression.h>
#include <Engine/Components.h>
#include <Projects/Project.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

namespace player {

namespace {

std::string jsonQuote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char escape[8];
            std::snprintf(escape, sizeof(escape), "\\u%04x", c);
            out += escape;
        } else {
            out += c;
        }
    }
    return out + "\"";
}
std::string number(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.6g", value);
    return text;
}
void write(const fs::path& json, const std::string& text) {
    if (json.empty()) {
        std::fputs(text.c_str(), stdout);
        return;
    }
    std::ofstream out(json);
    out << text;
    out.close();
    if (!out) throw std::runtime_error("can't write " + json.string());
}
// FNV-1a over the file's bytes: changes whenever the source does.
std::string contentHash(std::span<const uint8_t> bytes) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const uint8_t b : bytes) hash = (hash ^ b) * 0x100000001b3ull;
    char text[24];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}
const char* alphaName(shockwave::ModelMaterial::Alpha alpha) {
    switch (alpha) {
    case shockwave::ModelMaterial::Alpha::Mask: return "mask";
    case shockwave::ModelMaterial::Alpha::Blend: return "blend";
    default: return "opaque";
    }
}
shockwave::ModelData loadModelFile(const fs::path& path) {
    shockwave::ModelData model;
    std::string error;
    if (!shockwave::loadModel(path, model, error)) throw std::runtime_error(path.string() + ": " + error);
    return model;
}
// Size and bytes at each texture_quality, for a texture of this format and size.
// Fixed textures (UI, HDR environments) keep every level whatever the setting.
std::string
qualityJson(shockwave::TextureFormat format, uint32_t width, uint32_t height, uint32_t levels, bool followsQuality) {
    using shockwave::TextureQuality;
    std::string out = "{";
    bool first = true;
    for (const auto quality :
         {TextureQuality::Ultra, TextureQuality::High, TextureQuality::Medium, TextureQuality::Low}) {
        const uint32_t mip =
            followsQuality
                ? shockwave::residentFirstMip(shockwave::textureQualityFirstMip(quality), width, height, levels)
                : 0;
        out += (first ? "" : ", ") + jsonQuote(shockwave::textureQualityName(quality)) +
               ": {\"first_mip\": " + std::to_string(mip) +
               ", \"width\": " + std::to_string(std::max(width >> mip, 1u)) +
               ", \"height\": " + std::to_string(std::max(height >> mip, 1u)) +
               ", \"bytes\": " + std::to_string(shockwave::textureChainBytes(format, width, height, mip)) + "}";
        first = false;
    }
    return out + "}";
}
std::string textureJson(
    std::string_view source, shockwave::TextureFormat format, uint32_t width, uint32_t height, uint32_t levels,
    bool cookedLevels, std::string_view colour, std::string_view hash, bool followsQuality) {
    return "{\"source\": " + jsonQuote(source) + ", \"format\": " + jsonQuote(shockwave::textureFormatName(format)) +
           ", \"color_space\": " + jsonQuote(colour) + ", \"width\": " + std::to_string(width) +
           ", \"height\": " + std::to_string(height) + ", \"levels\": " + std::to_string(levels) +
           ", \"levels_from\": " + jsonQuote(cookedLevels ? "file" : "generated on the GPU") +
           ", \"source_hash\": " + jsonQuote(hash) +
           ", \"follows_texture_quality\": " + (followsQuality ? "true" : "false") +
           ", \"quality\": " + qualityJson(format, width, height, levels, followsQuality) + "}";
}

std::string stringArray(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) out += (i ? ", " : "") + jsonQuote(items[i]);
    return out + "]";
}
std::string bindingJson(const shockwave::ModelTextureBinding& b) {
    using namespace shockwave;
    return "\"uv_set\": " + std::to_string(b.uvSet) + ", \"transform\": {\"offset\": [" + number(b.transform.offset.x) +
           ", " + number(b.transform.offset.y) + "], \"scale\": [" + number(b.transform.scale.x) + ", " +
           number(b.transform.scale.y) + "], \"rotation\": " + number(b.transform.rotation) +
           "}, \"sampler\": {\"wrap_u\": " + jsonQuote(textureWrapName(b.sampler.wrapU)) +
           ", \"wrap_v\": " + jsonQuote(textureWrapName(b.sampler.wrapV)) +
           ", \"mag_filter\": " + jsonQuote(textureFilterName(b.sampler.magFilter)) +
           ", \"min_filter\": " + jsonQuote(textureFilterName(b.sampler.minFilter)) +
           ", \"mip_filter\": " + jsonQuote(b.sampler.mipmaps ? textureFilterName(b.sampler.mipFilter) : "none") +
           ", \"anisotropy\": " + std::to_string(b.sampler.maxAnisotropy) + "}";
}
// One resolved value, read through its descriptor.
std::string paramJson(const shockwave::MaterialParams& params, const shockwave::MaterialParamInfo& info) {
    using shockwave::MaterialValueType;
    const auto* base = reinterpret_cast<const unsigned char*>(&params) + info.offset;
    auto floats = [&](int n) {
        const auto* f = reinterpret_cast<const float*>(base);
        std::string out = "[";
        for (int i = 0; i < n; ++i) out += (i ? ", " : "") + number(f[i]);
        return out + "]";
    };
    switch (info.type) {
    case MaterialValueType::Float: return number(*reinterpret_cast<const float*>(base));
    case MaterialValueType::Vec2: return floats(2);
    case MaterialValueType::Color3: return floats(3);
    case MaterialValueType::Color4: return floats(4);
    case MaterialValueType::Bool: return *reinterpret_cast<const bool*>(base) ? "true" : "false";
    case MaterialValueType::Enum: {
        const int value = *reinterpret_cast<const int*>(base);
        return value >= 0 && size_t(value) < info.options.size() ? jsonQuote(info.options[size_t(value)])
                                                                 : std::to_string(value);
    }
    }
    return "null";
}
// The Content folder an asset is named from: its project's, or the folder it's in.
fs::path contentRoot(const fs::path& asset) {
    if (const auto manifest = shockwave::findProjectFile(asset)) {
        shockwave::Project project;
        std::string ignored;
        if (shockwave::loadProject(*manifest, project, ignored)) {
            std::error_code ec;
            const fs::path relative = fs::relative(fs::absolute(asset), fs::absolute(project.assetFolder), ec);
            if (!ec && !relative.empty() && *relative.begin() != "..") return project.assetFolder;
        }
    }
    return asset.parent_path().empty() ? fs::path(".") : asset.parent_path();
}

// A07-A09: the family, what it draws as at each quality, and what that costs.
std::string shadingJson(const shockwave::MaterialDesc& desc) {
    using namespace shockwave;
    const char* families[] = {"standard", "skin", "hair_card", "transmission"};
    std::string out = "{\"family\": " + jsonQuote(families[std::clamp(desc.params.family, 0, 3)]);
    const ShadingSettings defaults;
    ShadingSettings off;
    off.clearcoat = false;
    off.transmission = TransmissionQuality::Off;
    off.skin = SkinQuality::Off;
    off.hair = HairQuality::Low;
    ShadingSettings low = off;
    low.clearcoat = true;
    low.transmission = TransmissionQuality::Low;
    out += ", \"draws_as\": " + jsonQuote(materialEffectiveShading(desc, defaults));
    out += ", \"draws_as_low\": " + jsonQuote(materialEffectiveShading(desc, low));
    out += ", \"fallback\": " + jsonQuote(materialEffectiveShading(desc, off));
    const std::string cost = materialShadingCost(desc, defaults);
    out += ", \"passes\": " + jsonQuote(cost.empty() ? "none beyond the scene's" : cost) + "}";
    return out;
}

// --material-info for a .swmaterial: the chain it resolves through and the result.
int swmaterialInfo(const fs::path& asset, const fs::path& json) {
    using namespace shockwave;
    const fs::path root = contentRoot(asset);
    std::error_code ec;
    const std::string name = fs::relative(fs::absolute(asset), fs::absolute(root), ec).generic_string();
    MaterialDesc desc;
    std::string error;
    const MaterialProblem problem = resolveMaterial(name, folderMaterialLookup(root), desc, error);
    std::string out = "{\"asset\": " + jsonQuote(name) + ", \"content\": " + jsonQuote(root.generic_string()) +
                      ",\n \"problem\": " +
                      jsonQuote(
                          problem == MaterialProblem::None   ? "none"
                          : problem == MaterialProblem::File ? "file"
                                                             : "reference") +
                      ", \"error\": " + jsonQuote(error);
    if (problem == MaterialProblem::None) {
        std::vector<std::string> features;
        for (uint32_t bit = 0; bit < kMaterialFeatureCount; ++bit)
            if (desc.features & (1u << bit)) features.push_back(materialFeatureName(1u << bit));
        out += ",\n \"chain\": " + stringArray(desc.chain) + ",\n \"dependencies\": " + stringArray(desc.dependencies) +
               ",\n \"features\": " + stringArray(features) +
               ",\n \"low_quality_omits\": " + stringArray(materialLowQualityOmissions(desc)) +
               ",\n \"shading\": " + shadingJson(desc) + ",\n \"values\": {";
        bool first = true;
        for (const auto& info : materialParams()) {
            out += std::string(first ? "" : ",") + "\n  " + jsonQuote(info.key) + ": " + paramJson(desc.params, info);
            first = false;
        }
        out += "\n },\n \"slots\": [";
        first = true;
        for (size_t slot = 0; slot < kAllMaterialSlots; ++slot) {
            const ResolvedSlot& r = desc.slots[slot];
            if (r.texture.empty()) continue;
            const MaterialSlotInfo& info = materialSlotInfo(MaterialSlot(slot));
            out +=
                std::string(first ? "" : ",") + "\n  {\"slot\": " + jsonQuote(materialSlotName(MaterialSlot(slot))) +
                ", \"texture\": " + jsonQuote(r.texture.asset) +
                (r.texture.modelTexture >= 0 ? ", \"model_texture\": " + std::to_string(r.texture.modelTexture) : "") +
                ", \"color_space\": " + jsonQuote(info.srgb ? "srgb" : "linear") + ", " + bindingJson(r.binding) + "}";
            first = false;
        }
        out += first ? "]" : "\n ]";
        out += ",\n \"warnings\": " + stringArray(desc.warnings);
    }
    out += "}\n";
    write(json, out);
    return problem == MaterialProblem::None ? 0 : 1;
}

}   // namespace

int materialInfo(const fs::path& asset, const fs::path& json) {
    using namespace shockwave;
    if (isMaterialFile(asset)) return swmaterialInfo(asset, json);
    const ModelData model = loadModelFile(asset);
    std::string out = "{\"asset\": " + jsonQuote(asset.generic_string()) + ",\n \"materials\": [";
    for (size_t m = 0; m < model.materials.size(); ++m) {
        const ModelMaterial& material = model.materials[m];
        out += std::string(m ? "," : "") + "\n  {\"name\": " + jsonQuote(material.name) +
               ", \"alpha\": " + jsonQuote(alphaName(material.alpha)) +
               ", \"alpha_cutoff\": " + number(material.alphaCutoff) +
               ", \"double_sided\": " + (material.doubleSided ? "true" : "false") + ", \"slots\": [";
        bool firstSlot = true;
        const auto textures = materialTextures(material);
        for (size_t slot = 0; slot < kMaterialSlots; ++slot) {
            if (textures[slot] < 0) continue;
            const ModelTexture& texture = model.textures[textures[slot]];
            const ModelTextureBinding& b = material.bindings[slot];
            // Every mesh drawn with this material, and whether it has the UV set the slot reads.
            bool available = true;
            for (const auto& mesh : model.meshes)
                if (mesh.material == m && !mesh.data.indices.empty() && b.uvSet == 1 && mesh.data.uv1.empty())
                    available = false;
            out += std::string(firstSlot ? "" : ",") +
                   "\n    {\"slot\": " + jsonQuote(materialSlotName(MaterialSlot(slot))) +
                   ", \"texture\": " + jsonQuote(texture.source) +
                   ", \"color_space\": " + jsonQuote(texture.srgb ? "srgb" : "linear") +
                   ", \"uv_available\": " + (available ? "true" : "false") + ", " + bindingJson(b) + "}";
            firstSlot = false;
        }
        out += firstSlot ? "]}" : "\n  ]}";
    }
    out += "\n ],\n \"meshes\": [";
    bool firstMesh = true;
    for (const auto& mesh : model.meshes) {
        if (mesh.data.indices.empty()) continue;
        out += std::string(firstMesh ? "" : ",") + "\n  {\"name\": " + jsonQuote(mesh.name) +
               ", \"material\": " + std::to_string(mesh.material) +
               ", \"vertices\": " + std::to_string(mesh.data.vertices.size()) +
               ", \"uv_sets\": " + (mesh.data.uv1.empty() ? "1" : "2") + "}";
        firstMesh = false;
    }
    out += "\n ],\n \"warnings\": [";
    for (size_t i = 0; i < model.warnings.size(); ++i) out += (i ? ", " : "") + jsonQuote(model.warnings[i]);
    out += "]}\n";
    write(json, out);
    return 0;
}

int textureInfo(const fs::path& asset, const fs::path& json) {
    using namespace shockwave;
    std::vector<uint8_t> bytes;
    std::string error;
    // A .swimport next to the asset may make it a UI texture, which keeps its full size.
    ImportSettings settings;
    if (!loadImportSettings(importSettingsPath(asset), settings, error)) throw std::runtime_error(error);
    const bool material = settings.category == TextureCategory::Material;
    std::string out = "{\"asset\": " + jsonQuote(asset.generic_string()) + ",\n \"textures\": [";
    if (isModelFile(asset)) {
        // A model's textures, each once per colour space, as the importer reads them.
        const ModelData model = loadModelFile(asset);
        for (size_t i = 0; i < model.textures.size(); ++i) {
            const ModelTexture& t = model.textures[i];
            out += std::string(i ? "," : "") + "\n  " +
                   textureJson(
                       t.source, TextureFormat::RGBA8, t.image.width, t.image.height,
                       textureMipCount(t.image.width, t.image.height), false, t.srgb ? "srgb" : "linear",
                       contentHash(t.image.rgba), material);
        }
    } else {
        if (!readFileBytes(asset, bytes, error)) throw std::runtime_error(error);
        const std::string hash = contentHash(bytes);
        if (isKtx2File(asset)) {
            // Transcoded on the CPU, as a GPU without BC7 would get it.
            TextureData data;
            if (!transcodeKtx2(bytes, true, data, error)) throw std::runtime_error(asset.string() + ": " + error);
            out += "\n  " + textureJson(
                                asset.generic_string(), data.format, data.width, data.height,
                                uint32_t(data.levels.size()), true, data.srgb ? "srgb" : "linear", hash, material);
        } else if (isHdrImage(bytes)) {
            HdrImage hdr;
            if (!decodeHdrImage(bytes, hdr, error)) throw std::runtime_error(asset.string() + ": " + error);
            out += "\n  " + textureJson(
                                asset.generic_string(), TextureFormat::RGBA16F, hdr.width, hdr.height,
                                textureMipCount(hdr.width, hdr.height), false, "linear", hash, false);
        } else {
            Image image;
            if (!decodeImage(bytes, image, error)) throw std::runtime_error(asset.string() + ": " + error);
            out += "\n  " + textureJson(
                                asset.generic_string(), TextureFormat::RGBA8, image.width, image.height,
                                textureMipCount(image.width, image.height), false, "srgb", hash, material);
        }
    }
    out += "\n ]}\n";
    write(json, out);
    return 0;
}

int materialSchema(const fs::path& file) {
    const std::string schema = shockwave::materialSchemaMarkdown();
    if (file == "-") {
        std::fputs(schema.c_str(), stdout);
        return 0;
    }
    write(file, schema);
    return 0;
}

void Player::checkMaterialOverrides(
    shockwave::World& world, const std::string& scene, std::vector<std::string>& problems) {
    const std::string shownScene = m_project.relative(scene);
    world.each<const shockwave::ModelRenderer>([&](shockwave::Entity e, const shockwave::ModelRenderer& renderer) {
        if (renderer.materials.empty()) return;
        const shockwave::Name* name = world.get<shockwave::Name>(e);
        const std::string where = shownScene + ": entity '" + (name ? name->value : "?") + "': ModelRenderer";
        const auto model = engine().assets().model(renderer.model);
        if (!model) return;   // the scene check already reports a missing model
        for (const auto& o : renderer.materials) {
            const bool found = std::any_of(
                model->data.materials.begin(), model->data.materials.end(),
                [&](const shockwave::ModelMaterial& m) { return m.name == o.part; });
            if (!found)
                problems.push_back(
                    where + " overrides part '" + o.part + "', which " + engine().assets().modelName(renderer.model) +
                    " doesn't have (renamed in the modelling tool?)");
        }
    });
}

// A material that doesn't resolve was already reported, as a failed import.
void Player::checkMaterialFiles() {
    for (const auto& f : m_assets->files()) {
        if (f.kind != shockwave::AssetFolder::Kind::Material) continue;
        const std::string shown = m_project.relative(f.path);
        for (const std::string& warning : f.warnings) std::printf("warning %s: %s\n", shown.c_str(), warning.c_str());
        if (f.error.empty()) std::printf("ok %s\n", shown.c_str());
    }
}

}   // namespace player
