#pragma once

// Textures ready for the GPU in any format it can sample, with their mipmaps, for
// Renderer::createTexture(const TextureData&). Compressed textures come from .ktx2 files
// (Assets/TextureCompression.h, docs/Optimisation.md).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace shockwave {

enum class TextureFormat : uint8_t {
    RGBA8,   // 4 bytes per pixel, uncompressed
    BC7,   // 1 byte per pixel in 4 x 4 blocks; what desktop GPUs sample compressed colour from
    RGBA16F,   // 8 bytes per pixel, half floats: HDR images (.hdr environment maps); always linear
};
const char* textureFormatName(TextureFormat format);   // "RGBA8", "BC7", "RGBA16F"

// A plain 2D texture, or a cube map (G07): six square faces, +X, -X, +Y, -Y, +Z, -Z, in the
// orientation of Renderer/ReflectionProbes.h cubeFaceDirection. Cubes are sampled through a
// table of their own, never as 2D textures; a backend says which it has (supportsTextureShape).
enum class TextureShape : uint8_t { Texture2D, Cube };
const char* textureShapeName(TextureShape shape);   // "2d", "cube"

struct TextureData {
    struct Level {
        uint32_t width = 0, height = 0;
        size_t offset = 0, size = 0;   // in bytes
    };
    TextureFormat format = TextureFormat::RGBA8;
    bool srgb = true;   // colour textures are sRGB; data (normals, masks) is linear
    uint32_t width = 0, height = 0;
    // Largest first. An RGBA8 or RGBA16F 2D texture with a single level gets the rest made on
    // the GPU; a cube has exactly the levels given. A cube level is its six faces one after
    // another (each width x height, square), so its size is six faces' worth.
    std::vector<Level> levels;
    std::vector<uint8_t> bytes;
    TextureShape shape = TextureShape::Texture2D;   // last, so aggregate initialisers keep working

    uint32_t faces() const { return shape == TextureShape::Cube ? 6u : 1u; }
    // What it takes on the GPU, mipmaps included.
    size_t gpuBytes() const;
    // Every level's size is right for its format and fits in bytes.
    bool valid() const;
};

// The bytes one level of a format takes.
size_t textureLevelBytes(TextureFormat format, uint32_t width, uint32_t height);

// ---- Mip arithmetic for texture quality (A01, docs/Graphics/07-MathematicalContracts.md) ----
// Levels in a full chain down to 1 x 1.
uint32_t textureMipCount(uint32_t width, uint32_t height);
// Exact payload of levels firstMip.. to 1 x 1 (each max(1, size >> level)), block-rounded
// for BC7. A 4096-square BC7 chain is 22,369,648 bytes; from 512 down, 349,552.
uint64_t textureChainBytes(TextureFormat format, uint32_t width, uint32_t height, uint32_t firstMip = 0);
// The first level to keep for a wanted floor (textureQualityFirstMip): no further than the
// last of levels, and never below kMinResidentSize on the longer side unless the texture
// already is.
uint32_t residentFirstMip(uint32_t wantedFirstMip, uint32_t width, uint32_t height, uint32_t levels);
// An explicit level of detail in the source's terms, as the resident image numbers it:
// its level 0 is the source's level firstMip.
inline float residentLod(float sourceLod, uint32_t firstMip) {
    return sourceLod > float(firstMip) ? sourceLod - float(firstMip) : 0.0f;
}
// The texture from level firstMip down: those levels of a texture that has them, or for a
// lone RGBA8 level, that level made on the CPU by averaging 2 x 2 blocks (in linear light
// when srgb, so colour mips keep their brightness), its smaller levels left to the GPU as
// before. firstMip 0 returns the texture as it is. RGBA16F single levels aren't shrunk.
TextureData textureTail(const TextureData& source, uint32_t firstMip);

// ---- Alpha-tested textures (A02) ----
// The share of texels whose alpha is at least cutoff: what an alpha test keeps.
float alphaCoverage(const uint8_t* rgba, uint32_t width, uint32_t height, float cutoff);
// A full mip chain for one RGBA8 level, made on the CPU like textureTail's (2 x 2 boxes, in
// linear light when srgb). With cutoff >= 0, each smaller level's alpha is scaled so the
// same share of it passes an alpha test at cutoff as of level 0 (Castano 2010), so cut-out
// leaves and fences don't thin away or fill in with distance. Other formats, and textures
// that already have their levels, come back as they are.
TextureData textureMipChain(const TextureData& source, float coverageCutoff = -1.0f);

}   // namespace shockwave
