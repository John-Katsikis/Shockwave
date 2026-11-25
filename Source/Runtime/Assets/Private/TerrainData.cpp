// Heightmap terrain data: see Assets/TerrainData.h and docs/Terrain.md.
#include <Assets/TerrainData.h>

#include <Assets/Image.h>
#include <Renderer/Renderer.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace shockwave {

namespace fs = std::filesystem;

namespace {

// Words of a line, a quoted string kept whole (without its quotes); # starts a comment.
bool splitLine(std::string_view line, std::vector<std::string>& out, std::string& error) {
    out.clear();
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size() || line[i] == '#') break;
        if (line[i] == '"') {
            const size_t close = line.find('"', i + 1);
            if (close == std::string_view::npos) {
                error = "a quote isn't closed";
                return false;
            }
            out.emplace_back(line.substr(i + 1, close - i - 1));
            i = close + 1;
            continue;
        }
        size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
        out.emplace_back(line.substr(i, j - i));
        i = j;
    }
    return true;
}

bool number(const std::string& text, float& out) {
    const char* end = text.data() + text.size();
    auto [p, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc() && p == end && std::isfinite(out);
}

bool whole(const std::string& text, uint32_t& out) {
    const char* end = text.data() + text.size();
    auto [p, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc() && p == end;
}

// The shortest text that reads back as the same float.
std::string formatFloat(float v) {
    char text[32];
    for (int digits = 1; digits <= 9; ++digits) {
        std::snprintf(text, sizeof(text), "%.*g", digits, static_cast<double>(v));
        if (std::strtof(text, nullptr) == v &&
            (!std::strchr(text, 'e') || std::fabs(v) >= 1e9f || std::fabs(v) < 1e-4f))
            break;
    }
    return text;
}

bool readBytes(const fs::path& path, std::vector<uint8_t>& bytes, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "can't open " + path.generic_string();
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// Writes to a temporary name beside path, then renames it over path.
bool replaceFile(const fs::path& path, const void* data, size_t size, std::string& error) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    fs::path temporary = path;
    temporary += ".saving";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "can't write " + temporary.generic_string();
            return false;
        }
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!out) {
            error = "can't write " + temporary.generic_string();
            return false;
        }
    }
    fs::rename(temporary, path, ec);
    if (ec) {
        fs::remove(temporary, ec);
        error = "can't replace " + path.generic_string();
        return false;
    }
    return true;
}

constexpr float kMinSpacing = 0.1f, kMaxSpacing = 100.0f;
constexpr float kMaxHeight = 100000.0f;   // metres either way
constexpr float kMinUvMetres = 0.01f, kMaxUvMetres = 10000.0f;
// Level errors (metres) chosen so pickLod switches a chunk to half detail at about 150 m and
// quarter detail at about 400 m: a 1080p picture with a 60 degree field of view has about
// 935 pixels per metre at 1 m, and lod_error_pixels 1 allows error = distance / 935.
constexpr float kPixelsPerMetreAt1m = 935.0f;
constexpr float kHalfDetailMetres = 150.0f, kQuarterDetailMetres = 400.0f;

float smoothstep01(float t) { return t * t * (3.0f - 2.0f * t); }

}   // namespace

uint16_t TerrainData::value(float m) const {
    const float range = high - low;
    if (!(range > 0.0f)) return 0;
    const float v = std::round((m - low) / range * 65535.0f);
    return static_cast<uint16_t>(std::clamp(v, 0.0f, 65535.0f));
}

bool validTerrainSamples(uint32_t samples) {
    if (samples < kTerrainMinSamples || samples > kTerrainMaxSamples) return false;
    const uint32_t quads = samples - 1;
    return (quads & (quads - 1)) == 0;
}

bool isTerrainFile(const fs::path& path) { return path.extension() == ".swterrain"; }

bool isTerrainWeightsFile(const fs::path& path) {
    const std::string name = path.filename().string();
    constexpr std::string_view suffix = ".weights.png";
    return name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool parseTerrain(std::string_view text, TerrainData& terrain, std::string& error) {
    terrain = {};
    int lineNumber = 0;
    bool header = false;
    bool seenSamples = false, seenSpacing = false, seenRange = false, seenHeights = false, seenMaterial = false,
         seenUv = false, seenWeights = false;
    std::array<bool, kTerrainLayerCount> seenLayer{};
    std::vector<std::string> w;
    auto fail = [&](const std::string& why) {
        error = lineNumber > 0 ? "line " + std::to_string(lineNumber) + ": " + why : why;
        return false;
    };
    auto once = [&](bool& seen) {
        if (seen) return false;
        seen = true;
        return true;
    };
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t end = std::min(text.find('\n', pos), text.size());
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        std::string why;
        if (!splitLine(line, w, why)) return fail(why);
        if (w.empty()) {
            if (end == text.size()) break;
            continue;
        }
        const size_t count = w.size() - 1;
        if (!header) {
            if (w[0] != "swterrain") return fail("the first line must be \"swterrain 1\" or \"swterrain 2\"");
            if (w.size() != 2 || (w[1] != "1" && w[1] != "2")) return fail("only swterrain versions 1 and 2 are known");
            terrain.version = w[1] == "2" ? 2u : 1u;
            header = true;
        } else if (w[0] == "samples") {
            if (!once(seenSamples)) return fail("samples given twice");
            uint32_t n = 0;
            if (count != 1 || !whole(w[1], n)) return fail("samples needs one whole number");
            if (!validTerrainSamples(n)) return fail("samples must be 65, 129, 257, 513, 1025 or 2049");
            terrain.samples = n;
        } else if (w[0] == "spacing") {
            if (!once(seenSpacing)) return fail("spacing given twice");
            float v = 0;
            if (count != 1 || !number(w[1], v)) return fail("spacing needs one number");
            if (v < kMinSpacing || v > kMaxSpacing) return fail("spacing must be 0.1 to 100 metres");
            terrain.spacing = v;
        } else if (w[0] == "height_range") {
            if (!once(seenRange)) return fail("height_range given twice");
            float a = 0, b = 0;
            if (count != 2 || !number(w[1], a) || !number(w[2], b)) return fail("height_range needs two numbers");
            if (std::fabs(a) > kMaxHeight || std::fabs(b) > kMaxHeight)
                return fail("height_range must be within -100000 to 100000 metres");
            if (!(b > a)) return fail("height_range's high must be above its low");
            terrain.low = a;
            terrain.high = b;
        } else if (w[0] == "heights") {
            if (!once(seenHeights)) return fail("heights given twice");
            if (count != 1 || w[1].empty()) return fail("heights needs one quoted .r16 path");
            if (fs::path(w[1]).extension() != ".r16") return fail("heights must be a .r16 file");
            terrain.heightsFile = w[1];
        } else if (w[0] == "material") {
            if (terrain.version >= 2) return fail("swterrain 2 paints layers: use layer lines instead of material");
            if (!once(seenMaterial)) return fail("material given twice");
            if (count != 1) return fail("material needs one quoted .swmaterial path");
            if (!w[1].empty() && fs::path(w[1]).extension() != ".swmaterial")
                return fail("material must be a .swmaterial file");
            terrain.material = w[1];
        } else if (w[0] == "uv_metres") {
            if (terrain.version >= 2) return fail("swterrain 2 gives each layer its own metres per repeat");
            if (!once(seenUv)) return fail("uv_metres given twice");
            float v = 0;
            if (count != 1 || !number(w[1], v)) return fail("uv_metres needs one number");
            if (v < kMinUvMetres || v > kMaxUvMetres) return fail("uv_metres must be 0.01 to 10000");
            terrain.uvMetres = v;
        } else if (w[0] == "layer") {
            if (terrain.version < 2) return fail("layer needs \"swterrain 2\"");
            uint32_t index = 0;
            float uv = 0;
            if (count != 3 || !whole(w[1], index) || !number(w[3], uv))
                return fail("layer needs an index, a quoted .swmaterial path and metres per repeat");
            if (index >= kTerrainLayerCount) return fail("layer index must be 0 to 3");
            if (!once(seenLayer[index])) return fail("layer " + w[1] + " given twice");
            if (!w[2].empty() && fs::path(w[2]).extension() != ".swmaterial")
                return fail("a layer's material must be a .swmaterial file");
            if (uv < kMinUvMetres || uv > kMaxUvMetres)
                return fail("a layer's metres per repeat must be 0.01 to 10000");
            terrain.layers[index] = {true, w[2], uv};
        } else if (w[0] == "weights") {
            if (terrain.version < 2) return fail("weights needs \"swterrain 2\"");
            if (!once(seenWeights)) return fail("weights given twice");
            if (count != 1 || w[1].empty()) return fail("weights needs one quoted .png path");
            if (fs::path(w[1]).extension() != ".png") return fail("weights must be a .png file");
            terrain.weightsFile = w[1];
        } else {
            return fail("unknown key " + w[0]);
        }
        if (end == text.size()) break;
    }
    lineNumber = 0;
    if (!header) return fail("empty: the first line must be \"swterrain 1\"");
    if (!seenSamples) return fail("samples is missing");
    if (!seenSpacing) return fail("spacing is missing");
    if (!seenRange) return fail("height_range is missing");
    if (!seenHeights) return fail("heights is missing");
    if (terrain.version >= 2) {
        if (std::none_of(seenLayer.begin(), seenLayer.end(), [](bool b) { return b; }))
            return fail("swterrain 2 needs at least one layer line");
        if (!seenWeights) return fail("weights is missing");
    }
    return true;
}

std::string terrainText(const TerrainData& t) {
    std::string out = t.painted() ? "swterrain 2\n" : "swterrain 1\n";
    out += "samples " + std::to_string(t.samples) + "\n";
    out += "spacing " + formatFloat(t.spacing) + "\n";
    out += "height_range " + formatFloat(t.low) + " " + formatFloat(t.high) + "\n";
    out += "heights \"" + t.heightsFile + "\"\n";
    if (t.painted()) {
        for (uint32_t i = 0; i < kTerrainLayerCount; ++i)
            if (t.layers[i].used)
                out += "layer " + std::to_string(i) + " \"" + t.layers[i].material + "\" " +
                       formatFloat(t.layers[i].uvMetres) + "\n";
        out += "weights \"" + t.weightsFile + "\"\n";
        return out;
    }
    if (!t.material.empty()) out += "material \"" + t.material + "\"\n";
    out += "uv_metres " + formatFloat(t.uvMetres) + "\n";
    return out;
}

bool readTerrain(const fs::path& file, const fs::path& content, TerrainData& terrain, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!readBytes(file, bytes, error)) return false;
    TerrainData next;
    if (!parseTerrain(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), next, error))
        return false;
    const fs::path heights = content / next.heightsFile;
    std::vector<uint8_t> raw;
    if (!readBytes(heights, raw, error)) {
        error = "the heights file " + next.heightsFile + " is missing";
        return false;
    }
    const size_t want = size_t(next.samples) * next.samples * 2;
    if (raw.size() != want) {
        error = "the heights file " + next.heightsFile + " has " + std::to_string(raw.size()) + " bytes; " +
                std::to_string(next.samples) + " samples need " + std::to_string(want);
        return false;
    }
    next.heights.resize(want / 2);
    for (size_t i = 0; i < next.heights.size(); ++i)
        next.heights[i] = static_cast<uint16_t>(raw[i * 2] | (raw[i * 2 + 1] << 8));
    if (next.painted()) {
        std::vector<uint8_t> png;
        if (!readBytes(content / next.weightsFile, png, error)) {
            error = "the weights file " + next.weightsFile + " is missing";
            return false;
        }
        Image image;
        if (!decodeImage(png, image, error)) {
            error = "the weights file " + next.weightsFile + " can't be read: " + error;
            return false;
        }
        if (image.width != next.samples || image.height != next.samples) {
            error = "the weights file " + next.weightsFile + " is " + std::to_string(image.width) + " by " +
                    std::to_string(image.height) + "; " + std::to_string(next.samples) + " samples need " +
                    std::to_string(next.samples) + " by " + std::to_string(next.samples);
            return false;
        }
        next.weights = std::move(image.rgba);
        normaliseTerrainWeights(next);
    }
    terrain = std::move(next);
    return true;
}

bool writeTerrain(const fs::path& file, const fs::path& content, const TerrainData& terrain, std::string& error) {
    if (terrain.heights.size() != size_t(terrain.samples) * terrain.samples) {
        error = "the terrain has " + std::to_string(terrain.heights.size()) + " heights; " +
                std::to_string(terrain.samples) + " samples need " +
                std::to_string(size_t(terrain.samples) * terrain.samples);
        return false;
    }
    if (terrain.heightsFile.empty()) {
        error = "the terrain names no heights file";
        return false;
    }
    std::vector<uint8_t> raw(terrain.heights.size() * 2);
    for (size_t i = 0; i < terrain.heights.size(); ++i) {
        raw[i * 2] = static_cast<uint8_t>(terrain.heights[i] & 0xff);
        raw[i * 2 + 1] = static_cast<uint8_t>(terrain.heights[i] >> 8);
    }
    // Weights and heights first: a header naming new ones never points at a missing file.
    if (terrain.painted()) {
        if (terrain.weights.size() != size_t(terrain.samples) * terrain.samples * 4) {
            error = "the terrain's weights don't match its " + std::to_string(terrain.samples) + " samples";
            return false;
        }
        if (terrain.weightsFile.empty()) {
            error = "the terrain names no weights file";
            return false;
        }
        const std::vector<uint8_t> png = encodePngRgba8(terrain.samples, terrain.samples, terrain.weights);
        if (!replaceFile(content / terrain.weightsFile, png.data(), png.size(), error)) return false;
    }
    if (!replaceFile(content / terrain.heightsFile, raw.data(), raw.size(), error)) return false;
    const std::string text = terrainText(terrain);
    return replaceFile(file, text.data(), text.size(), error);
}

TerrainData flatTerrain(uint32_t samples, float spacing, float low, float high, float height) {
    TerrainData t;
    t.samples = samples;
    t.spacing = spacing;
    t.low = low;
    t.high = high;
    t.heights.assign(size_t(samples) * samples, t.value(height));
    return t;
}

namespace {

// Shares out total among the weights in proportion to share (integers summing to total), the
// largest remainders rounding up, ties to the lower layer.
void distribute(const float share[kTerrainLayerCount], uint32_t total, uint8_t out[kTerrainLayerCount]) {
    float sum = 0.0f;
    for (uint32_t i = 0; i < kTerrainLayerCount; ++i) sum += share[i];
    uint32_t given = 0;
    float rest[kTerrainLayerCount] = {};
    for (uint32_t i = 0; i < kTerrainLayerCount; ++i) {
        const float exact = sum > 0.0f ? share[i] / sum * float(total) : 0.0f;
        const auto whole = static_cast<uint32_t>(std::floor(exact));
        out[i] = static_cast<uint8_t>(whole);
        rest[i] = exact - float(whole);
        given += whole;
    }
    while (given < total && sum > 0.0f) {
        uint32_t best = kTerrainLayerCount;
        for (uint32_t i = 0; i < kTerrainLayerCount; ++i)
            if (share[i] > 0.0f && (best == kTerrainLayerCount || rest[i] > rest[best])) best = i;
        if (best == kTerrainLayerCount) break;
        ++out[best];
        rest[best] = -1.0f;
        ++given;
    }
}

uint32_t usedMask(const TerrainData& t) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < kTerrainLayerCount; ++i)
        if (t.layers[i].used) mask |= 1u << i;
    return mask;
}

}   // namespace

std::array<TerrainLayer, kTerrainLayerCount> terrainLayers(const TerrainData& t) {
    if (t.painted()) return t.layers;
    std::array<TerrainLayer, kTerrainLayerCount> one{};
    one[0] = {true, t.material, t.uvMetres};
    return one;
}

uint32_t terrainLayerCount(const TerrainData& t) {
    const auto layers = terrainLayers(t);
    return static_cast<uint32_t>(
        std::count_if(layers.begin(), layers.end(), [](const TerrainLayer& l) { return l.used; }));
}

void normaliseTerrainWeights(TerrainData& t) {
    if (!t.painted()) return;
    const size_t points = size_t(t.samples) * t.samples;
    t.weights.resize(points * 4, 0);
    const uint32_t used = usedMask(t);
    if (!used) return;
    const auto first = static_cast<uint32_t>(std::countr_zero(used));
    for (size_t p = 0; p < points; ++p) {
        uint8_t* w = &t.weights[p * 4];
        float share[kTerrainLayerCount];
        uint32_t sum = 0;
        for (uint32_t i = 0; i < kTerrainLayerCount; ++i) {
            if (!(used & (1u << i))) w[i] = 0;
            share[i] = float(w[i]);
            sum += w[i];
        }
        if (sum == 255) continue;
        if (sum == 0) {
            w[first] = 255;
            continue;
        }
        distribute(share, 255, w);
    }
}

int addTerrainLayer(TerrainData& t, std::string material, float uvMetres, std::string& error) {
    if (!material.empty() && fs::path(material).extension() != ".swmaterial") {
        error = "a layer's material must be a .swmaterial file";
        return -1;
    }
    if (!(uvMetres >= kMinUvMetres && uvMetres <= kMaxUvMetres)) {
        error = "a layer's metres per repeat must be 0.01 to 10000";
        return -1;
    }
    if (t.painted() && usedMask(t) == (1u << kTerrainLayerCount) - 1u) {
        error = "a terrain has at most 4 layers";
        return -1;
    }
    if (!t.painted()) {
        // Version 1 becomes version 2: its material is layer 0, everywhere.
        t.version = 2;
        t.layers = {};
        t.layers[0] = {true, t.material, t.uvMetres};
        t.material.clear();
        t.weightsFile = t.heightsFile.empty()
                            ? std::string()
                            : fs::path(t.heightsFile).replace_extension(".weights.png").generic_string();
        t.weights.assign(size_t(t.samples) * t.samples * 4, 0);
        for (size_t p = 0; p < size_t(t.samples) * t.samples; ++p) t.weights[p * 4] = 255;
    }
    for (uint32_t i = 0; i < kTerrainLayerCount; ++i)
        if (!t.layers[i].used) {
            t.layers[i] = {true, std::move(material), uvMetres};
            return int(i);
        }
    error = "a terrain has at most 4 layers";
    return -1;
}

bool removeTerrainLayer(TerrainData& t, uint32_t layer, std::string& error) {
    if (!t.painted()) {
        error = "a swterrain 1 terrain has only its one material";
        return false;
    }
    if (layer >= kTerrainLayerCount || !t.layers[layer].used) {
        error = "there's no layer " + std::to_string(layer);
        return false;
    }
    if (std::popcount(usedMask(t)) <= 1) {
        error = "a terrain keeps at least one layer";
        return false;
    }
    t.layers[layer] = {};
    normaliseTerrainWeights(t);   // its share goes to the others in proportion
    return true;
}

uint32_t terrainChunkLayers(const TerrainData& t, uint32_t chunk) {
    if (!t.painted()) return 1u;
    const size_t points = size_t(t.samples) * t.samples;
    if (t.weights.size() != points * 4) return usedMask(t);
    const uint32_t perSide = terrainChunksPerSide(t);
    if (perSide == 0) return usedMask(t);
    const uint32_t x0 = (chunk % perSide) * kTerrainChunkQuads, z0 = (chunk / perSide) * kTerrainChunkQuads;
    uint32_t mask = 0;
    for (uint32_t z = z0; z <= z0 + kTerrainChunkQuads && z < t.samples; ++z)
        for (uint32_t x = x0; x <= x0 + kTerrainChunkQuads && x < t.samples; ++x) {
            const uint8_t* w = &t.weights[(size_t(z) * t.samples + x) * 4];
            for (uint32_t i = 0; i < kTerrainLayerCount; ++i)
                if (w[i] >= 3) mask |= 1u << i;
        }
    return mask ? mask : usedMask(t);
}

float terrainHeight(const TerrainData& t, float x, float z) {
    if (t.heights.empty()) return t.low;
    const float last = float(t.samples - 1);
    const float fx = std::clamp(x / t.spacing, 0.0f, last), fz = std::clamp(z / t.spacing, 0.0f, last);
    const uint32_t x0 = std::min(uint32_t(fx), t.samples - 2), z0 = std::min(uint32_t(fz), t.samples - 2);
    const float u = fx - float(x0), v = fz - float(z0);
    const float a = t.at(x0, z0), b = t.at(x0 + 1, z0), c = t.at(x0, z0 + 1), d = t.at(x0 + 1, z0 + 1);
    return (a * (1 - u) + b * u) * (1 - v) + (c * (1 - u) + d * u) * v;
}

glm::vec3 terrainNormal(const TerrainData& t, uint32_t x, uint32_t z) {
    const uint32_t xl = x > 0 ? x - 1 : x, xr = std::min(x + 1, t.samples - 1);
    const uint32_t zl = z > 0 ? z - 1 : z, zr = std::min(z + 1, t.samples - 1);
    const float dx = (t.at(xr, z) - t.at(xl, z)) / (float(xr - xl) * t.spacing);
    const float dz = (t.at(x, zr) - t.at(x, zl)) / (float(zr - zl) * t.spacing);
    return glm::normalize(glm::vec3(-dx, 1.0f, -dz));
}

glm::vec3 terrainNormal(const TerrainData& t, float x, float z) {
    const float h = t.spacing;
    const float dx = (terrainHeight(t, x + h, z) - terrainHeight(t, x - h, z)) / (2.0f * h);
    const float dz = (terrainHeight(t, x, z + h) - terrainHeight(t, x, z - h)) / (2.0f * h);
    return glm::normalize(glm::vec3(-dx, 1.0f, -dz));
}

bool raycastTerrain(
    const TerrainData& t, const glm::vec3& origin, const glm::vec3& direction, float maxDistance, float& hit) {
    const float length = glm::length(direction);
    if (!(length > 0.0f) || t.heights.empty()) return false;
    const glm::vec3 d = direction / length;
    const float step = t.spacing * 0.5f, size = t.size();
    auto inside = [&](const glm::vec3& p) { return p.x >= 0 && p.z >= 0 && p.x <= size && p.z <= size; };
    auto above = [&](float s) {
        const glm::vec3 p = origin + d * s;
        return p.y - terrainHeight(t, p.x, p.z);
    };
    float previous = 0.0f;
    float previousGap = above(0.0f);
    bool previousInside = inside(origin);
    for (float s = step; s <= maxDistance + step; s += step) {
        const float at = std::min(s, maxDistance);
        const glm::vec3 p = origin + d * at;
        const bool in = inside(p);
        const float gap = above(at);
        if (in && previousInside && previousGap > 0.0f && gap <= 0.0f) {
            float a = previous, b = at;
            for (int i = 0; i < 16; ++i) {
                const float m = (a + b) * 0.5f;
                (above(m) > 0.0f ? a : b) = m;
            }
            hit = (a + b) * 0.5f / length;
            return true;
        }
        previous = at;
        previousGap = gap;
        previousInside = in;
        if (at >= maxDistance) break;
    }
    return false;
}

uint32_t terrainChunksPerSide(const TerrainData& t) { return (t.samples - 1) / kTerrainChunkQuads; }
uint32_t terrainChunkCount(const TerrainData& t) {
    const uint32_t n = terrainChunksPerSide(t);
    return n * n;
}

MeshData buildTerrainChunk(const TerrainData& t, uint32_t chunk) {
    const uint32_t per = terrainChunksPerSide(t);
    const uint32_t x0 = (chunk % per) * kTerrainChunkQuads, z0 = (chunk / per) * kTerrainChunkQuads;
    constexpr uint32_t side = kTerrainChunkQuads + 1;
    MeshData mesh;
    mesh.vertices.reserve(side * side + 4 * kTerrainChunkQuads);
    for (uint32_t z = 0; z < side; ++z)
        for (uint32_t x = 0; x < side; ++x) {
            Vertex v;
            const uint32_t px = x0 + x, pz = z0 + z;
            v.position = {float(px) * t.spacing, t.at(px, pz), float(pz) * t.spacing};
            v.normal = terrainNormal(t, px, pz);
            v.uv = glm::vec2(v.position.x, v.position.z) / t.uvMetres;
            const glm::vec3 tangent = glm::normalize(glm::vec3(1, 0, 0) - v.normal * v.normal.x);
            v.tangent = glm::vec4(tangent, 1.0f);
            mesh.vertices.push_back(v);
        }
    // The edge ring, counter-clockwise from above, and a dropped copy of each for the skirt.
    std::vector<uint32_t> ring;   // grid indices
    for (uint32_t x = 0; x < kTerrainChunkQuads; ++x) ring.push_back(x);   // z = 0, +X
    for (uint32_t z = 0; z < kTerrainChunkQuads; ++z) ring.push_back(z * side + kTerrainChunkQuads);   // x = end, +Z
    for (uint32_t x = kTerrainChunkQuads; x > 0; --x) ring.push_back(kTerrainChunkQuads * side + x);   // z = end, -X
    for (uint32_t z = kTerrainChunkQuads; z > 0; --z) ring.push_back(z * side);   // x = 0, -Z
    std::vector<uint32_t> dropped(side * side, UINT32_MAX);
    const float drop = 4.0f * t.spacing;
    for (const uint32_t i : ring) {
        Vertex v = mesh.vertices[i];
        v.position.y -= drop;
        dropped[i] = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(v);
    }
    auto grid = [&](uint32_t x, uint32_t z) { return z * side + x; };
    auto level = [&](uint32_t step, std::vector<uint32_t>& out) {
        for (uint32_t z = 0; z < kTerrainChunkQuads; z += step)
            for (uint32_t x = 0; x < kTerrainChunkQuads; x += step) {
                const uint32_t a = grid(x, z), b = grid(x + step, z), c = grid(x, z + step),
                               d = grid(x + step, z + step);
                out.insert(out.end(), {a, c, b, b, c, d});
            }
        // Skirt: the ring's points this level uses, joined to their dropped copies. Both
        // windings, so it hides gaps from either side whatever the material culls.
        std::vector<uint32_t> used;
        for (const uint32_t i : ring) {
            const uint32_t x = i % side, z = i / side;
            if (x % step == 0 && z % step == 0) used.push_back(i);
        }
        for (size_t k = 0; k < used.size(); ++k) {
            const uint32_t p = used[k], q = used[(k + 1) % used.size()];
            const uint32_t pd = dropped[p], qd = dropped[q];
            out.insert(out.end(), {p, pd, q, q, pd, qd});
            out.insert(out.end(), {p, q, pd, q, qd, pd});
        }
    };
    level(1, mesh.indices);
    const float errors[2] = {
        kHalfDetailMetres / kPixelsPerMetreAt1m,
        kQuarterDetailMetres / kPixelsPerMetreAt1m,
    };
    const uint32_t steps[2] = {2, 4};
    for (int i = 0; i < 2; ++i) {
        MeshLod lod;
        lod.firstIndex = static_cast<uint32_t>(mesh.lodIndices.size());
        level(steps[i], mesh.lodIndices);
        lod.indexCount = static_cast<uint32_t>(mesh.lodIndices.size()) - lod.firstIndex;
        lod.error = errors[i];
        mesh.lods.push_back(lod);
    }
    return mesh;
}

Bounds terrainChunkBounds(const TerrainData& t, uint32_t chunk) {
    const uint32_t per = terrainChunksPerSide(t);
    const uint32_t x0 = (chunk % per) * kTerrainChunkQuads, z0 = (chunk / per) * kTerrainChunkQuads;
    float lo = t.high, hi = t.low;
    for (uint32_t z = z0; z <= z0 + kTerrainChunkQuads; ++z)
        for (uint32_t x = x0; x <= x0 + kTerrainChunkQuads; ++x) {
            const float h = t.at(x, z);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
    Bounds b;
    b.min = {float(x0) * t.spacing, lo - 4.0f * t.spacing, float(z0) * t.spacing};
    b.max = {float(x0 + kTerrainChunkQuads) * t.spacing, hi, float(z0 + kTerrainChunkQuads) * t.spacing};
    return b;
}

std::vector<uint32_t> terrainChunksTouching(const TerrainData& t, glm::ivec4 rect) {
    std::vector<uint32_t> out;
    if (rect.x > rect.z || rect.y > rect.w) return out;
    const int per = int(terrainChunksPerSide(t));
    const int q = int(kTerrainChunkQuads);
    for (int cz = 0; cz < per; ++cz)
        for (int cx = 0; cx < per; ++cx) {
            // A chunk's points, plus the one-point border its normals read.
            const int ax = cx * q - 1, bx = cx * q + q + 1, az = cz * q - 1, bz = cz * q + q + 1;
            if (rect.x <= bx && rect.z >= ax && rect.y <= bz && rect.w >= az) out.push_back(uint32_t(cz * per + cx));
        }
    return out;
}

void terrainTriangles(
    const TerrainData& t, uint32_t step, std::vector<glm::vec3>& vertices, std::vector<uint32_t>& indices) {
    step = std::max(step, 1u);
    const uint32_t quads = (t.samples - 1) / step, side = quads + 1;
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    for (uint32_t z = 0; z < side; ++z)
        for (uint32_t x = 0; x < side; ++x)
            vertices.push_back({float(x * step) * t.spacing, t.at(x * step, z * step), float(z * step) * t.spacing});
    for (uint32_t z = 0; z < quads; ++z)
        for (uint32_t x = 0; x < quads; ++x) {
            const uint32_t a = base + z * side + x, b = a + 1, c = a + side, d = c + 1;
            indices.insert(indices.end(), {a, c, b, b, c, d});
        }
}

glm::ivec4 applyTerrainBrush(TerrainData& t, const TerrainBrushStroke& s) {
    glm::ivec4 changed{1, 1, 0, 0};
    if (t.heights.empty() || !(s.radius > 0.0f) || !(s.strength > 0.0f) || !(s.dt > 0.0f)) return changed;
    if (s.brush == TerrainBrush::Paint && (!t.painted() || s.layer >= kTerrainLayerCount || !t.layers[s.layer].used ||
                                           t.weights.size() != size_t(t.samples) * t.samples * 4))
        return changed;
    const int last = int(t.samples) - 1;
    const int x0 = std::max(0, int(std::ceil((s.centre.x - s.radius) / t.spacing)));
    const int x1 = std::min(last, int(std::floor((s.centre.x + s.radius) / t.spacing)));
    const int z0 = std::max(0, int(std::ceil((s.centre.y - s.radius) / t.spacing)));
    const int z1 = std::min(last, int(std::floor((s.centre.y + s.radius) / t.spacing)));
    if (x0 > x1 || z0 > z1) return changed;
    // Smooth reads the heights as they were before this application.
    std::vector<uint16_t> before;
    if (s.brush == TerrainBrush::Smooth) before = t.heights;
    const float strength = std::clamp(s.strength, 0.0f, 1.0f);
    const float rate = strength * s.radius * 0.5f * s.dt;   // Raise/Lower: metres this frame at the centre
    const float pull = std::min(1.0f, strength * 10.0f * s.dt);   // Smooth/Flatten: share of the way there
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            const float d = glm::length(glm::vec2(float(x) * t.spacing, float(z) * t.spacing) - s.centre);
            if (d >= s.radius) continue;
            const float lin = 1.0f - d / s.radius;
            const float weight = s.falloff == TerrainFalloff::Linear ? lin : smoothstep01(lin);
            if (s.brush == TerrainBrush::Paint) {
                // The layer's share moves this much of the way to all of it; the others give
                // up what it gains in proportion. At least one step, so it always gets there.
                uint8_t* w = &t.weights[(size_t(z) * t.samples + size_t(x)) * 4];
                const uint32_t l = s.layer;
                if (w[l] == 255) continue;
                const float a = std::min(1.0f, strength * 4.0f * s.dt * weight);
                auto mine = static_cast<uint32_t>(std::lround(float(w[l]) + a * float(255 - w[l])));
                if (mine <= w[l]) mine = w[l] + 1u;
                float share[kTerrainLayerCount];
                for (uint32_t i = 0; i < kTerrainLayerCount; ++i) share[i] = i == l ? 0.0f : float(w[i]);
                uint8_t next[kTerrainLayerCount];
                distribute(share, 255 - mine, next);
                next[l] = static_cast<uint8_t>(mine);
                if (std::equal(next, next + kTerrainLayerCount, w)) continue;
                std::copy(next, next + kTerrainLayerCount, w);
                changed = unionRect(changed, {x, z, x, z});
                continue;
            }
            uint16_t& value = t.heights[size_t(z) * t.samples + size_t(x)];
            const float h = t.metres(value);
            float next = h;
            switch (s.brush) {
            case TerrainBrush::Raise: next = h + rate * weight; break;
            case TerrainBrush::Lower: next = h - rate * weight; break;
            case TerrainBrush::Flatten: next = h + (s.flattenTo - h) * pull * weight; break;
            case TerrainBrush::Paint: break;   // above
            case TerrainBrush::Smooth: {
                float sum = 0.0f;
                int count = 0;
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, nz = z + dz;
                        if (nx < 0 || nz < 0 || nx > last || nz > last) continue;
                        sum += t.metres(before[size_t(nz) * t.samples + size_t(nx)]);
                        ++count;
                    }
                next = h + (sum / float(count) - h) * pull * weight;
                break;
            }
            }
            const uint16_t v = t.value(next);
            if (v == value) continue;
            value = v;
            changed = unionRect(changed, {x, z, x, z});
        }
    return changed;
}

std::vector<uint16_t> terrainRegion(const TerrainData& t, glm::ivec4 r) {
    std::vector<uint16_t> out;
    if (r.x > r.z || r.y > r.w) return out;
    out.reserve(size_t(r.z - r.x + 1) * size_t(r.w - r.y + 1));
    for (int z = r.y; z <= r.w; ++z)
        for (int x = r.x; x <= r.z; ++x) out.push_back(t.heights[size_t(z) * t.samples + size_t(x)]);
    return out;
}

void setTerrainRegion(TerrainData& t, glm::ivec4 r, std::span<const uint16_t> values) {
    if (r.x > r.z || r.y > r.w) return;
    size_t i = 0;
    for (int z = r.y; z <= r.w; ++z)
        for (int x = r.x; x <= r.z && i < values.size(); ++x)
            t.heights[size_t(z) * t.samples + size_t(x)] = values[i++];
}

glm::ivec4 unionRect(glm::ivec4 a, glm::ivec4 b) {
    if (a.x > a.z || a.y > a.w) return b;
    if (b.x > b.z || b.y > b.w) return a;
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.z, b.z), std::max(a.w, b.w)};
}

std::vector<uint8_t> terrainWeightsRegion(const TerrainData& t, glm::ivec4 r) {
    std::vector<uint8_t> out;
    if (r.x > r.z || r.y > r.w || t.weights.size() != size_t(t.samples) * t.samples * 4) return out;
    out.reserve(size_t(r.z - r.x + 1) * size_t(r.w - r.y + 1) * 4);
    for (int z = r.y; z <= r.w; ++z) {
        const auto* row = &t.weights[(size_t(z) * t.samples + size_t(r.x)) * 4];
        out.insert(out.end(), row, row + size_t(r.z - r.x + 1) * 4);
    }
    return out;
}

void setTerrainWeightsRegion(TerrainData& t, glm::ivec4 r, std::span<const uint8_t> values) {
    if (r.x > r.z || r.y > r.w || t.weights.size() != size_t(t.samples) * t.samples * 4) return;
    const size_t width = size_t(r.z - r.x + 1) * 4;
    if (values.size() < width * size_t(r.w - r.y + 1)) return;
    for (int z = r.y; z <= r.w; ++z)
        std::copy_n(
            values.begin() + ptrdiff_t(size_t(z - r.y) * width), width,
            t.weights.begin() + ptrdiff_t((size_t(z) * t.samples + size_t(r.x)) * 4));
}

TerrainLayout terrainLayout(const TerrainData& t) { return {t.version, t.layers, t.weightsFile}; }

TerrainEdit makeTerrainEdit(std::string asset, const TerrainData& before, const TerrainData& after, glm::ivec4 rect) {
    TerrainEdit edit;
    edit.asset = std::move(asset);
    edit.rect = rect;
    edit.before = terrainRegion(before, rect);
    edit.after = terrainRegion(after, rect);
    edit.layoutBefore = terrainLayout(before);
    edit.layoutAfter = terrainLayout(after);
    edit.layout = edit.layoutBefore != edit.layoutAfter;
    if (edit.layout) {
        // Layers added or removed: all of the weights, and version 1's material.
        edit.weightsBefore = before.weights;
        edit.weightsAfter = after.weights;
        edit.materialBefore = before.material;
        edit.materialAfter = after.material;
        edit.uvMetresBefore = before.uvMetres;
        edit.uvMetresAfter = after.uvMetres;
    } else if (before.painted()) {
        edit.weightsBefore = terrainWeightsRegion(before, rect);
        edit.weightsAfter = terrainWeightsRegion(after, rect);
    }
    return edit;
}

void applyTerrainEdit(TerrainData& terrain, const TerrainEdit& edit, bool redo) {
    setTerrainRegion(terrain, edit.rect, redo ? edit.after : edit.before);
    if (edit.layout) {
        const TerrainLayout& layout = redo ? edit.layoutAfter : edit.layoutBefore;
        terrain.version = layout.version;
        terrain.layers = layout.layers;
        terrain.weightsFile = layout.weightsFile;
        terrain.weights = redo ? edit.weightsAfter : edit.weightsBefore;
        terrain.material = redo ? edit.materialAfter : edit.materialBefore;
        terrain.uvMetres = redo ? edit.uvMetresAfter : edit.uvMetresBefore;
    } else {
        setTerrainWeightsRegion(terrain, edit.rect, redo ? edit.weightsAfter : edit.weightsBefore);
    }
}

bool importTerrainHeights(const fs::path& file, uint32_t& samples, std::vector<uint16_t>& heights, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!readBytes(file, bytes, error)) return false;
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".r16" || ext == ".raw") {
        const size_t count = bytes.size() / 2;
        const auto side = static_cast<uint32_t>(std::llround(std::sqrt(double(count))));
        if (bytes.size() % 2 != 0 || size_t(side) * side != count) {
            error = file.filename().generic_string() + " isn't a square of 16-bit values (" +
                    std::to_string(bytes.size()) + " bytes)";
            return false;
        }
        if (!validTerrainSamples(side)) {
            error = file.filename().generic_string() + " is " + std::to_string(side) + " by " + std::to_string(side) +
                    "; terrains are 65, 129, 257, 513, 1025 or 2049 points along a side";
            return false;
        }
        samples = side;
        heights.resize(count);
        for (size_t i = 0; i < count; ++i) heights[i] = static_cast<uint16_t>(bytes[i * 2] | (bytes[i * 2 + 1] << 8));
        return true;
    }
    GreyImage16 image;
    if (!decodeGreyImage16(bytes, image, error)) {
        error = file.filename().generic_string() + ": " + error;
        return false;
    }
    if (image.width != image.height || !validTerrainSamples(image.width)) {
        error = file.filename().generic_string() + " is " + std::to_string(image.width) + " by " +
                std::to_string(image.height) + "; terrains are square, 65, 129, 257, 513, 1025 or 2049 pixels a side";
        return false;
    }
    samples = image.width;
    heights = std::move(image.values);
    return true;
}

namespace {

TerrainChunk makeChunk(Renderer& renderer, const TerrainData& data, uint32_t c) {
    const MeshData mesh = buildTerrainChunk(data, c);
    TerrainChunk chunk;
    chunk.mesh = renderer.createMesh(mesh);
    if (chunk.mesh.valid()) renderer.setMeshLods(chunk.mesh, mesh.lods, mesh.lodIndices);
    chunk.bounds = computeBounds(mesh);
    chunk.layers = terrainChunkLayers(data, c);
    return chunk;
}

bool hasWeights(const TerrainData& d) { return d.painted() && d.weights.size() == size_t(d.samples) * d.samples * 4; }

// The weights texture: made the first time, then updated in place.
void syncWeights(Renderer& renderer, TerrainAsset& asset) {
    if (!hasWeights(*asset.data)) return;
    const uint32_t n = asset.data->samples;
    if (asset.weights.id != 0)
        renderer.updateTexture(asset.weights, n, n, asset.data->weights, false);
    else
        asset.weights = renderer.createTexture(n, n, asset.data->weights, false);
}

}   // namespace

std::shared_ptr<TerrainAsset> uploadTerrain(Renderer& renderer, std::shared_ptr<const TerrainData> data) {
    auto asset = std::make_shared<TerrainAsset>();
    asset->data = std::move(data);
    const uint32_t count = terrainChunkCount(*asset->data);
    asset->chunks.reserve(count);
    try {
        for (uint32_t c = 0; c < count; ++c) asset->chunks.push_back(makeChunk(renderer, *asset->data, c));
        syncWeights(renderer, *asset);
    } catch (...) {
        releaseTerrain(renderer, *asset);
        throw;
    }
    return asset;
}

std::shared_ptr<TerrainAsset> updateTerrain(
    Renderer& renderer, const TerrainAsset& previous, std::shared_ptr<const TerrainData> data, glm::ivec4 rect,
    std::vector<MeshHandle>& retired, uint32_t changed) {
    const bool sameShape =
        previous.data && previous.data->samples == data->samples && previous.chunks.size() == terrainChunkCount(*data);
    auto asset = std::make_shared<TerrainAsset>();
    asset->revision = previous.revision + 1;
    asset->weights = previous.weights;
    asset->data = std::move(data);
    if (!sameShape) {
        for (const TerrainChunk& c : previous.chunks)
            if (c.mesh.valid()) retired.push_back(c.mesh);
        const uint32_t count = terrainChunkCount(*asset->data);
        for (uint32_t c = 0; c < count; ++c) asset->chunks.push_back(makeChunk(renderer, *asset->data, c));
        syncWeights(renderer, *asset);
        return asset;
    }
    asset->chunks = previous.chunks;
    for (const uint32_t c : terrainChunksTouching(*asset->data, rect)) {
        if (changed & kTerrainHeightsChanged) {
            if (asset->chunks[c].mesh.valid()) retired.push_back(asset->chunks[c].mesh);
            asset->chunks[c] = makeChunk(renderer, *asset->data, c);
        } else if (changed & kTerrainWeightsChanged) {
            asset->chunks[c].layers = terrainChunkLayers(*asset->data, c);
        }
    }
    if (changed & kTerrainWeightsChanged) syncWeights(renderer, *asset);
    return asset;
}

void releaseTerrain(Renderer& renderer, const TerrainAsset& terrain) {
    for (const TerrainChunk& c : terrain.chunks)
        if (c.mesh.valid()) renderer.destroyMesh(c.mesh);
    if (terrain.weights.id != 0) renderer.destroyTexture(terrain.weights);
}

}   // namespace shockwave
