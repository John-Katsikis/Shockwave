// Asset optimisation in the player (docs/Optimisation.md): the project's settings, and
// --optimise, which compresses every texture into the cache now and reports what the
// textures and meshes cost.
#include "Player.h"

#include <Assets/AssetFolder.h>
#include <Assets/ImportSettings.h>
#include <Engine/Engine.h>

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace player {

namespace {
std::string megabytes(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}
// "4 levels: 2304 > 1152 > 576 > 288 triangles"
std::string levelText(const shockwave::MeshOptimiseReport& meshes) {
    std::string text = std::to_string(meshes.levels) + (meshes.levels == 1 ? " level: " : " levels: ");
    for (uint32_t i = 0; i < meshes.levels; ++i)
        text += (i ? " > " : "") + std::to_string(meshes.levelTriangles[i]);
    return text + " triangles";
}
}   // namespace

void Player::applyOptimisationSettings() {
    shockwave::ImportSettings defaults;
    shockwave::parseTextureCompression(m_project.textureCompression, defaults.compression);
    defaults.lods = static_cast<uint32_t>(m_project.meshLods);
    m_assets->setImportDefaults(defaults);
    m_assets->setCacheFolder(m_project.statePath() / "Cache" / "Textures");
    // Tools that quit straight away would leave compression half done; --optimise
    // finishes it on every thread instead.
    m_assets->setBackgroundCompression(m_tool.mode == Tool::Mode::Play);
    engine().renderer().setLodSettings({static_cast<float>(m_project.lodErrorPixels), m_tool.lodColors});
}

int Player::optimise() {
    if (const size_t waiting = m_assets->compressing())
        std::printf("Compressing %zu textures into %s...\n", waiting, m_project.relative(m_assets->cacheFolder()).c_str());
    else
        std::printf("Every texture is already in %s\n", m_project.relative(m_assets->cacheFolder()).c_str());
    int failures = 0;
    for (const auto& change : m_assets->finishCompressing())
        if (change.what == shockwave::AssetFolder::Change::What::Failed) ++failures;

    const bool bc7 = engine().renderer().supportsTextureFormat(shockwave::TextureFormat::BC7);
    uint64_t gpu = 0, uncompressed = 0, files = 0;
    uint32_t textures = 0, compressed = 0;
    std::ostringstream json;
    json << "{\n  \"gpu\": " << std::quoted(engine().renderer().gpuName()) << ",\n  \"bc7\": "
         << (bc7 ? "true" : "false") << ",\n  \"assets\": [";
    bool first = true;
    for (const auto& f : m_assets->files()) {
        using Kind = shockwave::AssetFolder::Kind;
        if (f.kind != Kind::Texture && f.kind != Kind::Model) continue;
        const std::string name = m_project.relative(f.path);
        if (!f.error.empty()) {
            std::printf("failed   %s: %s\n", name.c_str(), f.error.c_str());
            ++failures;
            continue;
        }
        gpu += f.gpuBytes;
        uncompressed += f.uncompressedBytes;
        files += f.compressedFileBytes;
        textures += f.textures;
        compressed += f.compressedTextures;
        const char* compression = shockwave::textureCompressionName(f.settings.compression);
        if (f.kind == Kind::Texture) {
            std::printf(
                "texture  %s  %ux%u  %s, %s on the GPU (%s uncompressed); compression %s\n", name.c_str(), f.width,
                f.height, f.compressedTextures ? "BC7" : "RGBA8", megabytes(f.gpuBytes).c_str(),
                megabytes(f.uncompressedBytes).c_str(), compression);
        } else {
            std::printf(
                "model    %s  %u meshes, %s; GPU shades %.2f vertices per triangle (%.2f before); %u of %u textures "
                "BC7, %s on the GPU (%s uncompressed)\n",
                name.c_str(), f.meshes.meshes, levelText(f.meshes).c_str(),
                static_cast<double>(f.meshes.shadedPerTriangleAfter),
                static_cast<double>(f.meshes.shadedPerTriangleBefore), f.compressedTextures, f.textures,
                megabytes(f.gpuBytes).c_str(), megabytes(f.uncompressedBytes).c_str());
        }
        for (const std::string& warning : f.warnings)
            if (warning.find("ompress") != std::string::npos) std::printf("         warning: %s\n", warning.c_str());
        json << (first ? "\n" : ",\n") << "    {\"name\": " << std::quoted(name) << ", \"kind\": \""
             << (f.kind == Kind::Texture ? "texture" : "model") << "\", \"compression\": \"" << compression
             << "\", \"textures\": " << f.textures << ", \"compressed_textures\": " << f.compressedTextures
             << ", \"gpu_bytes\": " << f.gpuBytes << ", \"uncompressed_bytes\": " << f.uncompressedBytes
             << ", \"compressed_file_bytes\": " << f.compressedFileBytes;
        if (f.kind == Kind::Model) {
            json << ", \"meshes\": " << f.meshes.meshes << ", \"lod_triangles\": [";
            for (uint32_t i = 0; i < f.meshes.levels; ++i) json << (i ? ", " : "") << f.meshes.levelTriangles[i];
            json << "], \"shaded_per_triangle_before\": " << f.meshes.shadedPerTriangleBefore
                 << ", \"shaded_per_triangle_after\": " << f.meshes.shadedPerTriangleAfter;
        }
        json << "}";
        first = false;
    }
    json << "\n  ],\n  \"textures\": " << textures << ",\n  \"compressed_textures\": " << compressed
         << ",\n  \"gpu_bytes\": " << gpu << ",\n  \"uncompressed_bytes\": " << uncompressed
         << ",\n  \"compressed_file_bytes\": " << files << ",\n  \"failures\": " << failures << "\n}\n";

    const double saved = uncompressed ? 100.0 * (1.0 - double(gpu) / double(uncompressed)) : 0.0;
    std::printf(
        "%u of %u textures compressed: %s on the GPU instead of %s (%.0f%% less); %s of .ktx2 files\n", compressed,
        textures, megabytes(gpu).c_str(), megabytes(uncompressed).c_str(), saved, megabytes(files).c_str());
    if (!bc7 && std::string_view(engine().renderer().backendName()) == "null")
        std::printf("note: no GPU in use, so sizes are shown as RGBA8; add --renderer vulkan to check yours\n");
    else if (!bc7)
        std::printf("note: this GPU (%s) can't sample BC7, so textures are cached compressed but drawn as RGBA8\n",
                    engine().renderer().gpuName());
    if (!m_tool.benchmarkJson.empty()) {
        std::ofstream out(m_tool.benchmarkJson);
        out << json.str();
        if (!out) {
            std::printf("couldn't write %s\n", m_tool.benchmarkJson.string().c_str());
            return 1;
        }
    }
    return failures ? 1 : 0;
}

}   // namespace player
