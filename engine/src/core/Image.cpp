#include <shockwave/Image.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <climits>
#include <memory>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO   // the engine reads the file, so paths work the same everywhere
#include <stb_image.h>

namespace shockwave {

bool isImageFile(const std::filesystem::path& path) {
    static constexpr std::array kExtensions = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd",
                                               ".gif", ".hdr", ".pnm", ".ppm", ".pgm"};
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
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4), stbi_image_free);
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

} // namespace shockwave
