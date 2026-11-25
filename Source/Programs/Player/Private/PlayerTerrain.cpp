// Terrain tools (W1a, docs/Terrain.md), no project needed:
//   --terrain-info      a terrain's points, size, heights and chunks, as text or JSON
//   --terrain-import    a .swterrain and its .r16 heights from a 16-bit heightmap
#include "Player.h"

#include <Assets/TerrainData.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace player {

namespace {

std::string quote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char escape[8];
            std::snprintf(escape, sizeof(escape), "\\u%04x", c);
            out += escape;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string number(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.6g", value);
    return text;
}

// The Content folder a file is in (the nearest parent called Content), else its own folder.
fs::path contentFolderOf(const fs::path& file) {
    const fs::path absolute = fs::absolute(file).lexically_normal();
    for (fs::path p = absolute.parent_path(); !p.empty() && p != p.parent_path(); p = p.parent_path())
        if (p.filename() == "Content") return p;
    return absolute.parent_path();
}

}   // namespace

int terrainInfo(const fs::path& file, const fs::path& json) {
    const fs::path content = contentFolderOf(file);
    const std::string name = fs::absolute(file).lexically_normal().lexically_relative(content).generic_string();
    shockwave::TerrainData terrain;
    std::string error;
    const bool ok = shockwave::readTerrain(file, content, terrain, error);
    std::string out = "{\"terrain\": " + quote(name) + ", \"valid\": " + (ok ? "true" : "false");
    float lowest = 0.0f, highest = 0.0f;
    if (ok) {
        const auto [lo, hi] = std::minmax_element(terrain.heights.begin(), terrain.heights.end());
        lowest = terrain.metres(*lo);
        highest = terrain.metres(*hi);
        out += ", \"samples\": " + std::to_string(terrain.samples) + ", \"spacing\": " + number(terrain.spacing) +
               ", \"size_metres\": " + number(terrain.size()) + ", \"height_range\": [" + number(terrain.low) + ", " +
               number(terrain.high) + "], \"min_height\": " + number(lowest) + ", \"max_height\": " + number(highest) +
               ", \"chunks\": " + std::to_string(shockwave::terrainChunkCount(terrain)) +
               ", \"heights\": " + quote(terrain.heightsFile) + ", \"material\": " + quote(terrain.material) +
               ", \"uv_metres\": " + number(terrain.uvMetres) + ", \"version\": " + std::to_string(terrain.version);
        std::vector<std::string> warnings;
        if (!terrain.material.empty() && !fs::exists(content / terrain.material))
            warnings.push_back("the material " + terrain.material + " isn't in Content");
        if (terrain.painted()) {
            // Each layer's share of the whole terrain, 0..1.
            double share[shockwave::kTerrainLayerCount] = {};
            const size_t points = size_t(terrain.samples) * terrain.samples;
            for (size_t p = 0; p < points; ++p)
                for (uint32_t i = 0; i < shockwave::kTerrainLayerCount; ++i) share[i] += terrain.weights[p * 4 + i];
            out += ", \"weights\": " + quote(terrain.weightsFile) + ", \"layers\": [";
            bool first = true;
            for (uint32_t i = 0; i < shockwave::kTerrainLayerCount; ++i) {
                const shockwave::TerrainLayer& l = terrain.layers[i];
                if (!l.used) continue;
                out += std::string(first ? "" : ", ") + "{\"index\": " + std::to_string(i) +
                       ", \"material\": " + quote(l.material) + ", \"uv_metres\": " + number(l.uvMetres) +
                       ", \"share\": " + number(share[i] / (255.0 * double(points))) + "}";
                first = false;
                if (!l.material.empty() && !fs::exists(content / l.material))
                    warnings.push_back(
                        "layer " + std::to_string(i) + "'s material " + l.material + " isn't in Content");
            }
            out += "]";
        }
        if (!warnings.empty()) {
            out += ", \"warnings\": [";
            for (size_t i = 0; i < warnings.size(); ++i) out += (i ? ", " : "") + quote(warnings[i]);
            out += "]";
        }
    } else {
        out += ", \"error\": " + quote(error);
    }
    out += "}\n";
    if (!json.empty()) {
        std::ofstream file(json, std::ios::binary | std::ios::trunc);
        file << out;
        if (!file) throw std::runtime_error("can't write " + json.string());
    }
    if (!ok) {
        std::printf("%s: %s\n", name.c_str(), error.c_str());
        return 1;
    }
    std::printf(
        "%s: %u points a side, %g m apart: %g m by %g m\n", name.c_str(), terrain.samples, double(terrain.spacing),
        double(terrain.size()), double(terrain.size()));
    std::printf(
        "  heights %g to %g m (range %g to %g m), %u chunks\n", double(lowest), double(highest), double(terrain.low),
        double(terrain.high), shockwave::terrainChunkCount(terrain));
    if (!terrain.painted()) {
        std::printf(
            "  heights file %s, material %s, %g m per texture repeat\n", terrain.heightsFile.c_str(),
            terrain.material.empty() ? "(default)" : terrain.material.c_str(), double(terrain.uvMetres));
        return 0;
    }
    std::printf(
        "  heights file %s, weights file %s, %u layers\n", terrain.heightsFile.c_str(), terrain.weightsFile.c_str(),
        shockwave::terrainLayerCount(terrain));
    const size_t points = size_t(terrain.samples) * terrain.samples;
    for (uint32_t i = 0; i < shockwave::kTerrainLayerCount; ++i) {
        const shockwave::TerrainLayer& l = terrain.layers[i];
        if (!l.used) continue;
        double share = 0;
        for (size_t p = 0; p < points; ++p) share += terrain.weights[p * 4 + i];
        std::printf(
            "  layer %u: %s, %g m per repeat, %.1f%% of the terrain\n", i,
            l.material.empty() ? "(default)" : l.material.c_str(), double(l.uvMetres),
            100.0 * share / (255.0 * double(points)));
    }
    return 0;
}

int terrainImport(const fs::path& heights, const fs::path& output, float spacing, float low, float high) {
    if (output.empty() || output.extension() != ".swterrain")
        throw UsageError("--terrain-import needs --output path/to/name.swterrain");
    uint32_t samples = 0;
    shockwave::TerrainData terrain;
    std::string error;
    if (!shockwave::importTerrainHeights(heights, samples, terrain.heights, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    terrain.samples = samples;
    terrain.spacing = spacing;
    terrain.low = low;
    terrain.high = high;
    const fs::path content = contentFolderOf(output);
    fs::path name = fs::absolute(output).lexically_normal().lexically_relative(content);
    terrain.heightsFile = fs::path(name).replace_extension(".r16").generic_string();
    if (!shockwave::writeTerrain(output, content, terrain, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf(
        "Wrote %s and %s: %u points a side, %g m\n", name.generic_string().c_str(), terrain.heightsFile.c_str(),
        samples, double(terrain.size()));
    return 0;
}

}   // namespace player
