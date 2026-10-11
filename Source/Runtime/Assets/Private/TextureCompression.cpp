#include <Assets/TextureCompression.h>

#include <Assets/Image.h>

#include <encoder/basisu_comp.h>
#include <transcoder/basisu_transcoder.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>

namespace shockwave {

namespace fs = std::filesystem;

namespace {
// Changing anything about how textures are encoded must change this, so caches rebuild.
constexpr std::string_view kEncoderVersion = "basisu 2.50; uastc faster, etc1s 128; mips wrap";

void initBasis() {
    static std::once_flag once;
    std::call_once(once, [] {
        basisu::basisu_encoder_init();   // also initialises the transcoder
        basist::basisu_transcoder_init();
    });
}

// 64-bit FNV-1a: plenty to tell versions of one asset apart.
uint64_t hashBytes(uint64_t hash, std::span<const uint8_t> bytes) {
    for (uint8_t byte : bytes) hash = (hash ^ byte) * 0x100000001b3ull;
    return hash;
}
}   // namespace

bool isKtx2File(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == ".ktx2";
}

bool encodeKtx2(
    const Image& image, TextureCompression mode, bool srgb, std::vector<uint8_t>& ktx2, std::string& error) {
    error.clear();
    if (mode == TextureCompression::Off) {
        error = "compression is off";
        return false;
    }
    if (!image.width || !image.height || image.rgba.size() < size_t(image.width) * image.height * 4) {
        error = "empty image";
        return false;
    }
    initBasis();
    // Mipmaps wrap at the edges, as tiling textures do; the encoder filters them in
    // linear light when srgb is set.
    uint32_t flags = uint32_t(basisu::cFlagKTX2) | uint32_t(basisu::cFlagGenMipsWrap) | (srgb ? uint32_t(basisu::cFlagSRGB) : 0u);
    basist::basis_tex_format format = basist::basis_tex_format::cUASTC_LDR_4x4;
    if (mode == TextureCompression::High) {
        flags |= uint32_t(basisu::cFlagKTX2UASTCSuperCompression) | uint32_t(basisu::cPackUASTCLevelFaster);
    } else {
        format = basist::basis_tex_format::cETC1S;
        flags |= uint32_t(basisu::BASISU_DEFAULT_QUALITY);
    }
    size_t size = 0;
    void* data = basisu::basis_compress(
        format, image.rgba.data(), image.width, image.height, image.width, flags, 0.0f, &size);
    if (!data) {
        error = "Basis Universal couldn't encode the texture";
        return false;
    }
    ktx2.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    basisu::basis_free_data(data);
    return true;
}

bool transcodeKtx2(std::span<const uint8_t> ktx2, bool bc7, TextureData& texture, std::string& error) {
    error.clear();
    initBasis();
    basist::ktx2_transcoder transcoder;
    if (ktx2.size() > UINT32_MAX || !transcoder.init(ktx2.data(), static_cast<uint32_t>(ktx2.size()))) {
        error = "not a valid .ktx2 file";
        return false;
    }
    if (!transcoder.is_etc1s() && !transcoder.is_uastc()) {
        error = "only Basis Universal ETC1S and UASTC .ktx2 files are supported (encode with `basisu -ktx2`)";
        return false;
    }
    if (transcoder.get_faces() != 1 || transcoder.get_layers() > 1) {
        error = "cube maps and texture arrays aren't supported yet";
        return false;
    }
    if (!transcoder.start_transcoding()) {
        error = "the .ktx2 file's data is damaged";
        return false;
    }
    TextureData result;
    result.format = bc7 ? TextureFormat::BC7 : TextureFormat::RGBA8;
    result.srgb = transcoder.is_srgb();
    result.width = transcoder.get_width();
    result.height = transcoder.get_height();
    const uint32_t levels = std::max(1u, transcoder.get_levels());
    for (uint32_t level = 0; level < levels; ++level) {
        basist::ktx2_image_level_info info{};
        if (!transcoder.get_image_level_info(info, level, 0, 0)) {
            error = "the .ktx2 file's level " + std::to_string(level) + " is missing";
            return false;
        }
        TextureData::Level out;
        out.width = info.m_orig_width;
        out.height = info.m_orig_height;
        out.offset = result.bytes.size();
        out.size = textureLevelBytes(result.format, out.width, out.height);
        result.bytes.resize(out.offset + out.size);
        const uint32_t units = bc7 ? info.m_total_blocks : out.width * out.height;
        if (!transcoder.transcode_image_level(
                level, 0, 0, result.bytes.data() + out.offset, units,
                bc7 ? basist::transcoder_texture_format::cTFBC7_RGBA : basist::transcoder_texture_format::cTFRGBA32)) {
            error = "the .ktx2 file's level " + std::to_string(level) + " couldn't be decoded";
            return false;
        }
        result.levels.push_back(out);
    }
    if (!result.valid()) {
        error = "the .ktx2 file's mipmaps aren't a complete chain";
        return false;
    }
    texture = std::move(result);
    return true;
}

uint64_t textureCacheKey(std::span<const uint8_t> source, TextureCompression mode) {
    uint64_t hash = 0xcbf29ce484222325ull;
    hash = hashBytes(hash, {reinterpret_cast<const uint8_t*>(kEncoderVersion.data()), kEncoderVersion.size()});
    const std::string_view name = textureCompressionName(mode);
    hash = hashBytes(hash, {reinterpret_cast<const uint8_t*>(name.data()), name.size()});
    return hashBytes(hash, source);
}

namespace {
std::string cacheStem(std::string_view asset) {
    std::string stem(asset);
    for (char& c : stem)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') c = '_';
    return stem;
}
}   // namespace

fs::path textureCachePath(const fs::path& folder, std::string_view asset, uint64_t key) {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(key));
    return folder / (cacheStem(asset) + "-" + hex + ".ktx2");
}

bool storeCachedTexture(const fs::path& file, std::span<const uint8_t> ktx2, std::string& error) {
    error.clear();
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path temp = file.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(ktx2.data()), static_cast<std::streamsize>(ktx2.size()));
        if (!out) {
            error = "can't write " + temp.string();
            return false;
        }
    }
    fs::rename(temp, file, ec);
    if (ec) {
        error = "can't write " + file.string() + ": " + ec.message();
        fs::remove(temp, ec);
        return false;
    }
    // Older copies of the same asset: "<stem>-<16 hex digits>.ktx2".
    const std::string name = file.filename().string();
    const std::string prefix = name.substr(0, name.size() - std::string_view("0123456789abcdef.ktx2").size());
    for (const auto& entry : fs::directory_iterator(file.parent_path(), ec)) {
        const std::string other = entry.path().filename().string();
        if (other != name && other.size() == name.size() && other.starts_with(prefix) && other.ends_with(".ktx2"))
            fs::remove(entry.path(), ec);
    }
    return true;
}

bool readFileBytes(const fs::path& file, std::vector<uint8_t>& bytes, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "can't open " + file.string();
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        error = file.string() + " is empty";
        return false;
    }
    return true;
}

}   // namespace shockwave
