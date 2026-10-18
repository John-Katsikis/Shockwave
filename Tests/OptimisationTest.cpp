// Mesh optimisation and texture compression (docs/Optimisation.md), without a GPU:
// levels of detail, vertex reuse, closed seams, .ktx2 encode and transcode, the texture
// cache, .swimport files and the project's keys.
#include <Assets/Image.h>
#include <Assets/ImportSettings.h>
#include <Assets/MeshOptimise.h>
#include <Assets/TextureCompression.h>
#include <Projects/Project.h>
#include <Renderer/Mesh.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <tuple>

using namespace shockwave;
namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

fs::path tempFolder(const char* name) {
    const fs::path root = fs::temp_directory_path() /
                          (std::string("shockwave-") + name + "-" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    return root;
}

std::string readText(const fs::path& file) {
    std::ifstream in(file);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// True when every edge of the triangles is shared by exactly two of them, comparing
// vertices by (rounded) position: no holes or cracks, seams included.
bool closedSurface(const MeshData& mesh, const uint32_t* indices, size_t count) {
    using Key = std::tuple<long, long, long>;
    auto key = [&](uint32_t v) {
        const glm::vec3 p = mesh.vertices[v].position * 1e4f;
        return Key{std::lround(p.x), std::lround(p.y), std::lround(p.z)};
    };
    std::map<std::pair<Key, Key>, int> edges;
    for (size_t t = 0; t + 2 < count; t += 3) {
        for (int e = 0; e < 3; ++e) {
            Key a = key(indices[t + e]), b = key(indices[t + (e + 1) % 3]);
            if (a == b) continue;   // a pole's degenerate edge
            if (b < a) std::swap(a, b);
            ++edges[{a, b}];
        }
    }
    return std::all_of(edges.begin(), edges.end(), [](const auto& edge) { return edge.second == 2; });
}

void meshes() {
    MeshData sphere = makeSphere(0.5f, 48, 24);
    const size_t fullIndices = sphere.indices.size();
    const MeshOptimiseReport report = optimiseMesh(sphere);
    check(sphere.indices.size() == fullIndices, "Reordering keeps every triangle");
    check(report.levels >= 3 && sphere.lods.size() == report.levels - 1, "A sphere gets several levels of detail");
    check(sphere.lods.size() <= kMaxMeshLods, "No more levels than the renderer takes");
    uint32_t previousCount = static_cast<uint32_t>(fullIndices);
    float previousError = 0.0f;
    bool shrinking = true, inRange = true, closed = true;
    for (const MeshLod& lod : sphere.lods) {
        shrinking &= lod.indexCount < previousCount && lod.error >= previousError;
        inRange &= lod.firstIndex + lod.indexCount <= sphere.lodIndices.size();
        for (uint32_t i = 0; i < lod.indexCount; ++i)
            inRange &= sphere.lodIndices[lod.firstIndex + i] < sphere.vertices.size();
        closed &= closedSurface(sphere, sphere.lodIndices.data() + lod.firstIndex, lod.indexCount);
        previousCount = lod.indexCount;
        previousError = lod.error;
    }
    check(shrinking, "Each level has fewer triangles and a larger error than the one before");
    check(inRange, "Levels index the mesh's own vertices");
    check(closed, "Simpler levels stay closed at the UV seam (no cracks)");
    check(sphere.lods.back().error < 0.5f, "Errors are in the mesh's units, smaller than the sphere");

    // Shuffled triangles reuse vertices badly; reordering fixes that.
    MeshData shuffled = makeSphere(0.5f, 48, 24);
    std::vector<std::array<uint32_t, 3>> triangles;
    for (size_t i = 0; i < shuffled.indices.size(); i += 3)
        triangles.push_back({shuffled.indices[i], shuffled.indices[i + 1], shuffled.indices[i + 2]});
    std::shuffle(triangles.begin(), triangles.end(), std::mt19937(7));
    shuffled.indices.clear();
    for (const auto& t : triangles) shuffled.indices.insert(shuffled.indices.end(), t.begin(), t.end());
    const MeshOptimiseReport reordered = optimiseMesh(shuffled, {.lods = 0});
    check(shuffled.lods.empty(), "lods 0 makes no levels");
    check(reordered.shadedPerTriangleAfter < reordered.shadedPerTriangleBefore * 0.5f,
          "Reordering at least halves the vertices shaded per triangle");

    MeshData cube = makeCube(1.0f);
    optimiseMesh(cube);
    check(cube.lods.empty(), "Small meshes get no levels");
}

Image gradient(uint32_t size) {
    Image image{size, size, std::vector<uint8_t>(size_t(size) * size * 4)};
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) {
            uint8_t* p = &image.rgba[(size_t(y) * size + x) * 4];
            p[0] = uint8_t(x * 255 / (size - 1));
            p[1] = uint8_t(y * 255 / (size - 1));
            p[2] = 128;
            p[3] = 255;
        }
    return image;
}

void textures() {
    const Image image = gradient(64);
    std::vector<uint8_t> ktx2;
    std::string error;
    check(encodeKtx2(image, TextureCompression::High, true, ktx2, error), "High encodes");
    check(ktx2.size() > 12 && ktx2[1] == 'K' && ktx2[2] == 'T' && ktx2[3] == 'X', "The result is a .ktx2 file");

    TextureData bc7;
    check(transcodeKtx2(ktx2, true, bc7, error), "Transcodes to BC7");
    check(bc7.format == TextureFormat::BC7 && bc7.width == 64 && bc7.height == 64, "BC7 at full size");
    check(bc7.levels.size() == 7, "With every mipmap down to 1x1");
    check(!bc7.levels.empty() && bc7.levels[0].size == 16 * 16 * 16, "16 bytes per 4x4 block");
    check(bc7.srgb, "Colour textures stay sRGB");

    TextureData rgba;
    check(transcodeKtx2(ktx2, false, rgba, error), "Transcodes to RGBA8 for GPUs without BC7");
    check(rgba.format == TextureFormat::RGBA8 && rgba.levels.size() == 7 && rgba.levels[0].size == 64 * 64 * 4,
          "RGBA8 with mipmaps");
    int worst = 0;
    for (size_t i = 0; i < size_t(64) * 64 * 4 && i < rgba.bytes.size(); ++i)
        worst = std::max(worst, std::abs(int(rgba.bytes[i]) - int(image.rgba[i])));
    check(worst <= 16, "High stays close to the original");
    check(bc7.gpuBytes() * 3 < rgba.gpuBytes(), "BC7 takes about a quarter of RGBA8's memory");

    std::vector<uint8_t> small;
    check(encodeKtx2(image, TextureCompression::Small, false, small, error), "Small encodes");
    TextureData smallData;
    check(transcodeKtx2(small, true, smallData, error) && !smallData.srgb, "Small transcodes, as data (linear)");
    check(!encodeKtx2(image, TextureCompression::Off, true, small, error), "Off doesn't encode");
    check(!transcodeKtx2(std::vector<uint8_t>{1, 2, 3}, true, bc7, error) && !error.empty(), "Bad files fail");

    // The cache: a key per source and mode, and only the newest copy of an asset kept.
    const std::vector<uint8_t> a{1, 2, 3}, b{1, 2, 4};
    check(textureCacheKey(a, TextureCompression::High) != textureCacheKey(b, TextureCompression::High),
          "Edited textures get a new cache key");
    check(textureCacheKey(a, TextureCompression::High) != textureCacheKey(a, TextureCompression::Small),
          "A different mode gets a new cache key");
    const fs::path folder = tempFolder("texture-cache");
    const fs::path first = textureCachePath(folder, "textures/wood.png", 1);
    const fs::path second = textureCachePath(folder, "textures/wood.png", 2);
    const fs::path other = textureCachePath(folder, "model.glb#0", 3);
    check(first.filename() == "textures_wood.png-0000000000000001.ktx2", "Cache files are named after the asset");
    check(storeCachedTexture(first, ktx2, error) && storeCachedTexture(other, ktx2, error), "Cache writes");
    check(storeCachedTexture(second, ktx2, error), "Cache rewrites");
    check(!fs::exists(first) && fs::exists(second) && fs::exists(other), "Older copies of the asset are removed");
    std::vector<uint8_t> read;
    check(readFileBytes(second, read, error) && read == ktx2, "Cache reads back");
    fs::remove_all(folder);
}

void importSettings() {
    const fs::path folder = tempFolder("swimport");
    const fs::path asset = folder / "bricks.png";
    std::ofstream(asset) << "png";
    const ImportSettings defaults;
    std::string error;
    check(saveImportSettings(asset, defaults, defaults, error), "Saving defaults works");
    check(!fs::exists(importSettingsPath(asset)), "Defaults write no file");
    ImportSettings custom;
    custom.compression = TextureCompression::Small;
    check(saveImportSettings(asset, custom, defaults, error), "Saving an override works");
    const std::string text = readText(importSettingsPath(asset));
    check(text.find("shockwave_import 1") != std::string::npos && text.find("compression small") != std::string::npos &&
              text.find("lods") == std::string::npos,
          "Only what differs is written");
    ImportSettings loaded;
    check(loadImportSettings(importSettingsPath(asset), loaded, error) && loaded == custom, "Overrides load back");
    check(isImportSettingsFile(importSettingsPath(asset)) && !isImportSettingsFile(asset), ".swimport is recognised");

    for (const char* bad : {"shockwave_import 1\ncompression tiny\n", "shockwave_import 1\nlods 9\n",
                            "shockwave_import 1\nsharpen 1\n", "shockwave_import 2\n"}) {
        std::ofstream(importSettingsPath(asset)) << bad;
        ImportSettings untouched;
        check(!loadImportSettings(importSettingsPath(asset), untouched, error) && untouched == ImportSettings{},
              "Bad settings fail and change nothing");
        check(error.find(":") != std::string::npos, "Errors name the file and line");
    }
    ImportSettings none;
    check(loadImportSettings(folder / "missing.png.swimport", none, error), "No file is fine");
    check(saveImportSettings(asset, defaults, defaults, error) && !fs::exists(importSettingsPath(asset)),
          "Saving defaults again removes the file");
    fs::remove_all(folder);
}

void projectKeys() {
    const fs::path folder = tempFolder("optimisation-project");
    std::string error;
    Project project;
    check(createProject(folder, "Optimised", "optimised", project, error), "Project created");
    fs::create_directories(project.startupScene.parent_path());
    std::ofstream(project.startupScene) << "shockwave_scene 2\n";
    const fs::path file = project.file;
    check(loadProject(file, project, error), "Project loads");
    check(project.textureCompression == "high" && project.meshLods == 4 && project.lodErrorPixels == 1.0,
          "Defaults: high, 4 levels, 1 pixel");
    check(readText(file).find("texture_compression") == std::string::npos, "Defaults aren't written");
    project.textureCompression = "small";
    project.meshLods = 2;
    project.lodErrorPixels = 2.5;
    check(saveProject(project, error), "Project saves");
    Project reloaded;
    check(loadProject(file, reloaded, error) && reloaded.textureCompression == "small" && reloaded.meshLods == 2 &&
              reloaded.lodErrorPixels == 2.5,
          "Optimisation keys round-trip");
    for (const char* bad : {"texture_compression tiny\n", "mesh_lods 8\n", "lod_error_pixels -1\n"}) {
        std::ofstream(file, std::ios::app) << bad;
        Project broken;
        check(!loadProject(file, broken, error), "Bad optimisation keys fail");
        check(saveProject(project, error), "Project saves again");
    }
    fs::remove_all(folder);
}

// HDR images (environment maps): full range from a Radiance file, and the RGBA16F format
// they're uploaded in.
void hdrImages() {
    // 2 x 1 pixels, flat (not run-length encoded) RGBE: 1.0 and 8.0.
    std::string file = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
    for (const unsigned char byte : {128, 128, 128, 129, 128, 128, 128, 132}) file += static_cast<char>(byte);
    const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(file.data()), file.size());
    check(isHdrImage(bytes), "A Radiance header is HDR");
    HdrImage hdr;
    std::string error;
    check(decodeHdrImage(bytes, hdr, error), "The HDR image decodes");
    check(hdr.width == 2 && hdr.height == 1 && hdr.rgba.size() == 8, "HDR size");
    check(
        hdr.rgba.size() == 8 && std::abs(hdr.rgba[0] - 1.0f) < 1e-3f && std::abs(hdr.rgba[4] - 8.0f) < 1e-3f,
        "HDR values above 1 survive");
    const uint8_t png[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    check(!isHdrImage(png), "A PNG isn't HDR");

    check(textureLevelBytes(TextureFormat::RGBA16F, 4, 2) == 64, "RGBA16F is 8 bytes a pixel");
    TextureData half{TextureFormat::RGBA16F, false, 4, 2, {{4, 2, 0, 64}}, std::vector<uint8_t>(64)};
    check(half.valid() && half.gpuBytes() == 64 + 64 / 3, "A lone RGBA16F level gets mipmaps");
    check(std::string(textureFormatName(TextureFormat::RGBA16F)) == "RGBA16F", "RGBA16F's name");
}
}   // namespace

int main() {
    meshes();
    textures();
    hdrImages();
    importSettings();
    projectKeys();
    if (failures == 0) std::puts("optimisation: all checks passed");
    return failures ? 1 : 0;
}
