#include <shockwave/Image.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>

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
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                            static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (!pixels) {
        error = path.string() + ": " + stbi_failure_reason();
        return false;
    }
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    image.rgba.assign(pixels, pixels + size_t(width) * height * 4);
    stbi_image_free(pixels);
    return true;
}

} // namespace shockwave
