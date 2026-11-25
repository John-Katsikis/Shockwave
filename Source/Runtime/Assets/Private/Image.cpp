#include <Assets/Image.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <fstream>
#include <memory>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO   // the engine reads the file, so paths work the same everywhere
#include <stb_image.h>

namespace shockwave {

bool isImageFile(const std::filesystem::path& path) {
    static constexpr std::array kExtensions = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd",
                                               ".gif", ".hdr", ".pnm",  ".ppm", ".pgm"};
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return std::find(kExtensions.begin(), kExtensions.end(), ext) != kExtensions.end();
}

bool loadImage(const std::filesystem::path& path, Image& image, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "can't open " + path.string();
        return false;
    }
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        error = path.string() + " is empty";
        return false;
    }
    return decodeImage({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()}, image, error);
}

bool decodeImage(std::span<const uint8_t> bytes, Image& image, std::string& error) {
    error.clear();
    if (bytes.empty() || bytes.size() > size_t(INT_MAX)) {
        error = "empty image or encoded image exceeds decoder limit";
        return false;
    }
    int width = 0, height = 0, channels = 0;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4),
        stbi_image_free);
    if (!pixels) {
        const char* reason = stbi_failure_reason();
        error = reason ? reason : "image decoding failed";
        return false;
    }
    Image decoded;
    decoded.width = static_cast<uint32_t>(width);
    decoded.height = static_cast<uint32_t>(height);
    decoded.rgba.assign(pixels.get(), pixels.get() + size_t(width) * height * 4);
    image = std::move(decoded);
    return true;
}

bool decodeGreyImage16(std::span<const uint8_t> bytes, GreyImage16& image, std::string& error) {
    error.clear();
    if (bytes.empty() || bytes.size() > size_t(INT_MAX)) {
        error = "empty image or encoded image exceeds decoder limit";
        return false;
    }
    int width = 0, height = 0, channels = 0;
    std::unique_ptr<stbi_us, decltype(&stbi_image_free)> pixels(
        stbi_load_16_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 1),
        stbi_image_free);
    if (!pixels) {
        const char* reason = stbi_failure_reason();
        error = reason ? reason : "image decoding failed";
        return false;
    }
    GreyImage16 decoded;
    decoded.width = static_cast<uint32_t>(width);
    decoded.height = static_cast<uint32_t>(height);
    decoded.values.assign(pixels.get(), pixels.get() + size_t(width) * height);
    image = std::move(decoded);
    return true;
}

bool isHdrImage(std::span<const uint8_t> bytes) {
    return !bytes.empty() && bytes.size() <= size_t(INT_MAX) &&
           stbi_is_hdr_from_memory(bytes.data(), static_cast<int>(bytes.size()));
}

bool decodeHdrImage(std::span<const uint8_t> bytes, HdrImage& image, std::string& error) {
    error.clear();
    if (!isHdrImage(bytes)) {
        error = "not a Radiance .hdr image";
        return false;
    }
    int width = 0, height = 0, channels = 0;
    std::unique_ptr<float, decltype(&stbi_image_free)> pixels(
        stbi_loadf_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4),
        stbi_image_free);
    if (!pixels) {
        const char* reason = stbi_failure_reason();
        error = reason ? reason : "image decoding failed";
        return false;
    }
    HdrImage decoded;
    decoded.width = static_cast<uint32_t>(width);
    decoded.height = static_cast<uint32_t>(height);
    decoded.rgba.assign(pixels.get(), pixels.get() + size_t(width) * height * 4);
    image = std::move(decoded);
    return true;
}

namespace {

uint32_t pngCrc(const uint8_t* data, size_t size, uint32_t crc = 0xffffffffu) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return crc;
}

void putBig32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

void pngChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
    putBig32(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    putBig32(out, pngCrc(out.data() + start, out.size() - start) ^ 0xffffffffu);
}

}   // namespace

std::vector<uint8_t> encodePngRgba8(uint32_t width, uint32_t height, std::span<const uint8_t> rgba) {
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> header;
    putBig32(header, width);
    putBig32(header, height);
    header.insert(header.end(), {8, 6, 0, 0, 0});   // 8 bits, RGBA, deflate, adaptive filters, no interlace
    pngChunk(out, "IHDR", header);
    // Each row: filter 0 (none), then the pixels.
    const size_t row = size_t(width) * 4;
    std::vector<uint8_t> raw;
    raw.reserve((row + 1) * height);
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        const size_t at = size_t(y) * row;
        if (at + row <= rgba.size())
            raw.insert(raw.end(), rgba.begin() + ptrdiff_t(at), rgba.begin() + ptrdiff_t(at + row));
        else
            raw.resize(raw.size() + row, 0);
    }
    // zlib: stored deflate blocks of up to 65535 bytes, then the Adler-32 of the raw data.
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + ptrdiff_t(pos), raw.begin() + ptrdiff_t(pos + n));
        pos += n;
    } while (pos < raw.size());
    uint32_t a = 1, b = 0;
    for (const uint8_t c : raw) {
        a = (a + c) % 65521u;
        b = (b + a) % 65521u;
    }
    putBig32(z, (b << 16) | a);
    pngChunk(out, "IDAT", z);
    pngChunk(out, "IEND", {});
    return out;
}

}   // namespace shockwave
