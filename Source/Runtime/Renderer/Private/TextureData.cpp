#include <Renderer/TextureData.h>

#include <Renderer/RenderTypes.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace shockwave {

const char* textureFormatName(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGBA8: return "RGBA8";
    case TextureFormat::BC7: return "BC7";
    case TextureFormat::RGBA16F: return "RGBA16F";
    }
    return "unknown";
}

size_t textureLevelBytes(TextureFormat format, uint32_t width, uint32_t height) {
    switch (format) {
    case TextureFormat::RGBA8: return size_t(width) * height * 4;
    case TextureFormat::BC7: return size_t((width + 3) / 4) * ((height + 3) / 4) * 16;
    case TextureFormat::RGBA16F: return size_t(width) * height * 8;
    }
    return 0;
}

size_t TextureData::gpuBytes() const {
    // A lone uncompressed level gets the rest of its chain made on the GPU.
    if (format != TextureFormat::BC7 && levels.size() == 1) return textureChainBytes(format, width, height);
    size_t total = 0;
    for (const Level& level : levels) total += textureLevelBytes(format, level.width, level.height);
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

uint32_t textureMipCount(uint32_t width, uint32_t height) {
    uint32_t levels = 1;
    for (uint32_t size = std::max(width, height); size > 1; size >>= 1) ++levels;
    return levels;
}

uint64_t textureChainBytes(TextureFormat format, uint32_t width, uint32_t height, uint32_t firstMip) {
    uint64_t total = 0;
    for (uint32_t level = firstMip; level < textureMipCount(width, height); ++level)
        total += textureLevelBytes(format, std::max(1u, width >> level), std::max(1u, height >> level));
    return total;
}

uint32_t residentFirstMip(uint32_t wanted, uint32_t width, uint32_t height, uint32_t levels) {
    uint32_t first = std::min(wanted, levels ? levels - 1 : 0);
    while (first > 0 && std::max(width, height) >> first < kMinResidentSize) --first;
    return first;
}

namespace {
// sRGB to linear light and back, exactly as the GPU decodes (IEC 61966-2-1).
const std::array<float, 256>& srgbToLinear() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = float(i) / 255.0f;
            t[size_t(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return table;
}
uint8_t linearToSrgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    const float encoded = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(encoded * 255.0f));
}
}   // namespace

namespace {
// One level as floats (linear light for sRGB colour; alpha is always linear), and back.
std::vector<float> decodeLevel(const uint8_t* bytes, size_t count, bool srgb) {
    const auto& decode = srgbToLinear();
    std::vector<float> pixels(count);
    for (size_t i = 0; i < count; ++i) pixels[i] = srgb && i % 4 != 3 ? decode[bytes[i]] : float(bytes[i]) / 255.0f;
    return pixels;
}
void encodeLevel(const std::vector<float>& pixels, bool srgb, std::vector<uint8_t>& out) {
    for (size_t i = 0; i < pixels.size(); ++i)
        out.push_back(
            srgb && i % 4 != 3 ? linearToSrgb(pixels[i])
                               : static_cast<uint8_t>(std::lround(std::clamp(pixels[i], 0.0f, 1.0f) * 255.0f)));
}
// The next level down: each texel the mean of a 2 x 2 block (edge texels repeat on odd sizes).
std::vector<float> halve(const std::vector<float>& pixels, uint32_t& w, uint32_t& h) {
    const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
    std::vector<float> next(size_t(nw) * nh * 4);
    for (uint32_t y = 0; y < nh; ++y)
        for (uint32_t x = 0; x < nw; ++x)
            for (uint32_t c = 0; c < 4; ++c) {
                const uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
                const uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
                const auto at = [&](uint32_t px, uint32_t py) { return pixels[(size_t(py) * w + px) * 4 + c]; };
                next[(size_t(y) * nw + x) * 4 + c] = (at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1)) * 0.25f;
            }
    w = nw;
    h = nh;
    return next;
}
float coverage(const std::vector<float>& pixels, float scale, float cutoff) {
    size_t kept = 0;
    for (size_t i = 3; i < pixels.size(); i += 4) kept += std::min(pixels[i] * scale, 1.0f) >= cutoff;
    return pixels.empty() ? 0.0f : float(kept) / float(pixels.size() / 4);
}
}   // namespace

float alphaCoverage(const uint8_t* rgba, uint32_t width, uint32_t height, float cutoff) {
    size_t kept = 0;
    const size_t texels = size_t(width) * height;
    for (size_t i = 0; i < texels; ++i) kept += float(rgba[i * 4 + 3]) / 255.0f >= cutoff;
    return texels ? float(kept) / float(texels) : 0.0f;
}

TextureData textureMipChain(const TextureData& source, float coverageCutoff) {
    if (source.format != TextureFormat::RGBA8 || source.levels.size() != 1 || !source.valid()) return source;
    TextureData chain;
    chain.format = source.format;
    chain.srgb = source.srgb;
    chain.width = source.width;
    chain.height = source.height;
    uint32_t w = source.width, h = source.height;
    const uint8_t* level0 = source.bytes.data() + source.levels[0].offset;
    chain.bytes.assign(level0, level0 + source.levels[0].size);
    chain.levels.push_back({w, h, 0, source.levels[0].size});
    std::vector<float> pixels = decodeLevel(level0, source.levels[0].size, source.srgb);
    // Quantised like the stored level, so a cutoff of exactly 0.5 sees what the GPU sees.
    const float target = coverageCutoff >= 0.0f ? coverage(pixels, 1.0f, coverageCutoff) : 0.0f;
    while (w > 1 || h > 1) {
        pixels = halve(pixels, w, h);   // always from the unscaled level above
        std::vector<float> stored = pixels;
        if (coverageCutoff > 0.0f) {
            // Coverage only grows with the scale: bisect for the one that matches level 0.
            float lo = 0.0f, hi = 1.0f / std::max(coverageCutoff, 1e-3f) * 4.0f;
            for (int step = 0; step < 24; ++step) {
                const float mid = 0.5f * (lo + hi);
                (coverage(pixels, mid, coverageCutoff) < target ? lo : hi) = mid;
            }
            const float scale = 0.5f * (lo + hi);
            for (size_t i = 3; i < stored.size(); i += 4) stored[i] = std::min(stored[i] * scale, 1.0f);
        }
        const size_t offset = chain.bytes.size();
        encodeLevel(stored, source.srgb, chain.bytes);
        chain.levels.push_back({w, h, offset, chain.bytes.size() - offset});
    }
    return chain;
}

TextureData textureTail(const TextureData& source, uint32_t firstMip) {
    if (firstMip == 0 || source.levels.empty()) return source;
    TextureData tail;
    tail.format = source.format;
    tail.srgb = source.srgb;
    if (source.levels.size() > 1) {
        // Already has its levels: keep those from firstMip down.
        firstMip = std::min<uint32_t>(firstMip, uint32_t(source.levels.size()) - 1);
        tail.width = source.levels[firstMip].width;
        tail.height = source.levels[firstMip].height;
        for (size_t i = firstMip; i < source.levels.size(); ++i) {
            const auto& level = source.levels[i];
            tail.levels.push_back({level.width, level.height, tail.bytes.size(), level.size});
            tail.bytes.insert(
                tail.bytes.end(), source.bytes.begin() + ptrdiff_t(level.offset),
                source.bytes.begin() + ptrdiff_t(level.offset + level.size));
        }
        return tail;
    }
    if (source.format != TextureFormat::RGBA8) return source;
    // One RGBA8 level: halve it firstMip times in floating point, rounding only at the end.
    uint32_t w = source.width, h = source.height;
    firstMip = std::min(firstMip, textureMipCount(w, h) - 1);
    std::vector<float> pixels =
        decodeLevel(source.bytes.data() + source.levels[0].offset, size_t(w) * h * 4, source.srgb);
    for (uint32_t level = 0; level < firstMip; ++level) pixels = halve(pixels, w, h);
    tail.width = w;
    tail.height = h;
    encodeLevel(pixels, source.srgb, tail.bytes);
    tail.levels = {{w, h, 0, tail.bytes.size()}};
    return tail;
}

}   // namespace shockwave
