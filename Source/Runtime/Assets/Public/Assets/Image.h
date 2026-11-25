#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace shockwave {

// Decoded pixels, ready for Renderer::createTexture.
struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;   // 4 bytes per pixel, rows top to bottom
};

// Reads PNG, JPEG, TGA, BMP, PSD, GIF (first frame), HDR (brought down to 8 bits) and
// PNM files. Safe to call from many threads at once.
bool loadImage(const std::filesystem::path& path, Image& image, std::string& error);
bool decodeImage(std::span<const uint8_t> bytes, Image& image, std::string& error);
// One 16-bit grey channel per pixel, rows top to bottom (terrain heights, W1a). 8-bit images
// are widened (0..255 to 0..65535); colour images are averaged to grey.
struct GreyImage16 {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint16_t> values;
};
bool decodeGreyImage16(std::span<const uint8_t> bytes, GreyImage16& image, std::string& error);
// Whether loadImage knows the file's extension.
bool isImageFile(const std::filesystem::path& path);

// Radiance .hdr pixels at full range, for environment maps (docs/Lighting.md).
struct HdrImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> rgba;   // linear, 4 floats per pixel, rows top to bottom
};
// Whether the bytes are a Radiance .hdr file (by its header, not its name).
bool isHdrImage(std::span<const uint8_t> bytes);
bool decodeHdrImage(std::span<const uint8_t> bytes, HdrImage& image, std::string& error);

}   // namespace shockwave
