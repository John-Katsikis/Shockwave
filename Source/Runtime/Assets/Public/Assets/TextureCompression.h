#pragma once

// Texture compression with Basis Universal (docs/Optimisation.md). A texture is encoded
// once into a small .ktx2 file, which is slow, and kept in the project's cache. Loading
// one turns it into BC7, the compressed format desktop GPUs sample directly, in a few
// milliseconds: a quarter of RGBA8's memory and bandwidth, mipmaps included.

#include <Assets/ImportSettings.h>
#include <Renderer/TextureData.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

struct Image;

// Encodes RGBA8 pixels as a .ktx2 file with its mipmaps: UASTC (High) or ETC1S (Small),
// Zstandard-compressed. Uses only the calling thread; seconds for a big texture, so run it
// in the background. srgb: colour (true) or data such as normals and masks (false).
bool encodeKtx2(
    const Image& image, TextureCompression mode, bool srgb, std::vector<uint8_t>& ktx2, std::string& error);
// Turns a .ktx2 file into GPU-ready levels: BC7 when bc7 is true (Renderer::
// supportsTextureFormat), else RGBA8. Fast. Any thread.
bool transcodeKtx2(std::span<const uint8_t> ktx2, bool bc7, TextureData& texture, std::string& error);
bool isKtx2File(const std::filesystem::path& path);

// ---- The texture cache: compressed copies of a project's textures between runs ----
// Files are named after the asset and a key made from its source bytes and how it's
// compressed, so an edited texture or a changed setting never picks up a stale copy.
// srgb: whether it's encoded as colour or data (encodeKtx2).
uint64_t textureCacheKey(std::span<const uint8_t> source, TextureCompression mode, bool srgb = true);
// <folder>/<asset name with / as _>-<key>.ktx2. Model textures pass "model.glb#2".
std::filesystem::path textureCachePath(const std::filesystem::path& folder, std::string_view asset, uint64_t key);
// Writes through a temporary file, then removes the asset's older copies.
bool storeCachedTexture(const std::filesystem::path& file, std::span<const uint8_t> ktx2, std::string& error);
bool readFileBytes(const std::filesystem::path& file, std::vector<uint8_t>& bytes, std::string& error);

}   // namespace shockwave
