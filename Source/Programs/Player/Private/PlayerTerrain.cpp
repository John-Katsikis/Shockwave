// Terrain tools (W1a, docs/Terrain.md), no project needed:
//   --terrain-info      a terrain's points, size, heights and chunks, as text or JSON
//   --terrain-import    a .swterrain and its .r16 heights from a 16-bit heightmap
#include "Player.h"

#include <Assets/TerrainData.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

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
               ", \"uv_metres\": " + number(terrain.uvMetres);
        if (!terrain.material.empty() && !fs::exists(content / terrain.material))
            out += ", \"warnings\": [" + quote("the material " + terrain.material + " isn't in Content") + "]";
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
    std::printf(
        "  heights file %s, material %s, %g m per texture repeat\n", terrain.heightsFile.c_str(),
        terrain.material.empty() ? "(default)" : terrain.material.c_str(), double(terrain.uvMetres));
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
