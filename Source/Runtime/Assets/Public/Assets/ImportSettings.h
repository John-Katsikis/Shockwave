#pragma once

// How an asset is imported (docs/Optimisation.md). The project sets the defaults
// (texture_compression, mesh_lods in the .swproject); a .swimport file next to an asset
// overrides them for that asset alone:
//
//     # bricks.png.swimport
//     shockwave_import 1
//     compression small
//     category ui
//
// The editor writes these files from the Optimisation window; people and tools can write
// them by hand. Saving one re-imports its asset.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace shockwave {

enum class TextureCompression : uint8_t {
    Off,   // RGBA8: 4 bytes per pixel on the GPU
    High,   // UASTC, sampled as BC7: a quarter of the memory, close to the original
    Small,   // ETC1S, sampled as BC7: same GPU memory, much smaller files, softer detail
};
const char* textureCompressionName(TextureCompression mode);   // "off", "high", "small"
bool parseTextureCompression(std::string_view name, TextureCompression& mode);

// What a texture is for (A01). Material textures follow the texture_quality setting; UI
// textures (menu art, icons, anything read as text) always keep their full size.
enum class TextureCategory : uint8_t { Material, Ui };
const char* textureCategoryName(TextureCategory category);   // "material", "ui"
bool parseTextureCategory(std::string_view name, TextureCategory& category);

struct ImportSettings {
    TextureCompression compression = TextureCompression::High;   // textures, and a model's textures
    uint32_t lods = 4;   // models: simpler levels of detail to make, 0..7
    TextureCategory category = TextureCategory::Material;   // textures, and a model's textures
    bool operator==(const ImportSettings&) const = default;
};

// "<asset>.swimport", next to the asset.
std::filesystem::path importSettingsPath(const std::filesystem::path& asset);
bool isImportSettingsFile(const std::filesystem::path& path);
// Applies the file's overrides to settings. No file is fine: settings stay as they are.
// Strict: an unknown key or a bad value fails with its line number, leaving settings alone.
bool loadImportSettings(const std::filesystem::path& file, ImportSettings& settings, std::string& error);
// Writes what differs from defaults to the asset's .swimport file, or removes the file
// when nothing does.
bool saveImportSettings(
    const std::filesystem::path& asset, const ImportSettings& settings, const ImportSettings& defaults,
    std::string& error);

}   // namespace shockwave
