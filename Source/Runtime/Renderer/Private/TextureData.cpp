#include <Renderer/TextureData.h>

#include <algorithm>

namespace shockwave {

const char* textureFormatName(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGBA8: return "RGBA8";
    case TextureFormat::BC7: return "BC7";
    }
    return "unknown";
}

size_t textureLevelBytes(TextureFormat format, uint32_t width, uint32_t height) {
    switch (format) {
    case TextureFormat::RGBA8: return size_t(width) * height * 4;
    case TextureFormat::BC7: return size_t((width + 3) / 4) * ((height + 3) / 4) * 16;
    }
    return 0;
}

size_t TextureData::gpuBytes() const {
    size_t total = 0;
    for (const Level& level : levels) total += textureLevelBytes(format, level.width, level.height);
    // A lone RGBA8 level gets its mipmaps made on the GPU: a third more again.
    if (format == TextureFormat::RGBA8 && levels.size() == 1) total += total / 3;
    return total;
}

bool TextureData::valid() const {
    if (!width || !height || levels.empty() || levels[0].width != width || levels[0].height != height) return false;
    for (size_t i = 0; i < levels.size(); ++i) {
        const Level& level = levels[i];
        const uint32_t w = std::max(1u, width >> i), h = std::max(1u, height >> i);
        if (level.width != w || level.height != h || level.size != textureLevelBytes(format, w, h) ||
            level.offset > bytes.size() || level.size > bytes.size() - level.offset)
            return false;
    }
    return true;
}

}   // namespace shockwave
