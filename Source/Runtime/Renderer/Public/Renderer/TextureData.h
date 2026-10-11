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
};
const char* textureFormatName(TextureFormat format);   // "RGBA8", "BC7"

struct TextureData {
    struct Level {
        uint32_t width = 0, height = 0;
        size_t offset = 0, size = 0;   // in bytes
    };
    TextureFormat format = TextureFormat::RGBA8;
    bool srgb = true;   // colour textures are sRGB; data (normals, masks) is linear
    uint32_t width = 0, height = 0;
    // Largest first. An RGBA8 texture with a single level gets the rest made on the GPU.
    std::vector<Level> levels;
    std::vector<uint8_t> bytes;

    // What it takes on the GPU, mipmaps included.
    size_t gpuBytes() const;
    // Every level's size is right for its format and fits in bytes.
    bool valid() const;
};

// The bytes one level of a format takes.
size_t textureLevelBytes(TextureFormat format, uint32_t width, uint32_t height);

}   // namespace shockwave
