// Heightmap terrain data (W1a, docs/Terrain.md): the .swterrain format and its errors,
// heights between and beyond the points, render chunks and their levels, brushes and undo,
// and importing raw heights. No GPU.
#include <Assets/TerrainData.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace shockwave;
namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool value, const std::string& message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}
bool near(float a, float b, float eps = 1e-3f) { return std::abs(a - b) <= eps; }

const char* kGood = "swterrain 1\n"
                    "# a comment\n"
                    "samples 129\n"
                    "spacing 2\n"
                    "height_range -10 90\n"
                    "heights \"terrain/hills.r16\"\n"
                    "material \"materials/grass.swmaterial\"\n"
                    "uv_metres 8\n";

void format() {
    TerrainData t;
    std::string error;
    check(parseTerrain(kGood, t, error), "the example parses: " + error);
    check(t.samples == 129 && t.spacing == 2.0f && t.low == -10.0f && t.high == 90.0f, "values read");
    check(t.heightsFile == "terrain/hills.r16" && t.material == "materials/grass.swmaterial", "paths read");
    check(t.uvMetres == 8.0f, "uv_metres read");
    TerrainData again;
    check(parseTerrain(terrainText(t), again, error), "the canonical text parses: " + error);
    check(terrainText(again) == terrainText(t), "the canonical text round-trips");

    // Every error names its line.
    struct Bad {
        std::string text;
        std::string expect;
    };
    const Bad bad[] = {
        {"swterrain 2\n", "line 1: "},
        {"swterrain 1\nsamples 100\n", "line 2: samples must be"},
        {"swterrain 1\nsamples 129\nsamples 129\n", "line 3: samples given twice"},
        {"swterrain 1\nspacing 0\n", "line 2: spacing must be"},
        {"swterrain 1\nheight_range 5 5\n", "line 2: height_range's high"},
        {"swterrain 1\nheights \"a.png\"\n", "line 2: heights must be a .r16"},
        {"swterrain 1\nmaterial \"a.png\"\n", "line 2: material must be"},
        {"swterrain 1\nuv_metres 0\n", "line 2: uv_metres must be"},
        {"swterrain 1\n\nbogus 1\n", "line 3: unknown key bogus"},
        {"swterrain 1\nheights \"a.r16\n", "line 2: a quote isn't closed"},
        {"swterrain 1\nsamples 129\nspacing 1\nheight_range 0 1\n", "heights is missing"},
        {"", "empty"},
    };
    for (const Bad& b : bad) {
        TerrainData ignored;
        const bool ok = parseTerrain(b.text, ignored, error);
        check(
            !ok && error.find(b.expect) != std::string::npos,
            "\"" + b.text + "\" fails with \"" + b.expect + "\", got \"" + error + "\"");
    }
    for (uint32_t n : {65u, 129u, 257u, 513u, 1025u, 2049u})
        check(validTerrainSamples(n), std::to_string(n) + " samples");
    for (uint32_t n : {33u, 64u, 66u, 100u, 4097u}) check(!validTerrainSamples(n), std::to_string(n) + " refused");
}

TerrainData slope() {
    // Height = x metres along X (spacing 1), flat along Z.
    TerrainData t = flatTerrain(65, 1.0f, 0.0f, 64.0f, 0.0f);
    for (uint32_t z = 0; z < t.samples; ++z)
        for (uint32_t x = 0; x < t.samples; ++x) t.heights[z * t.samples + x] = t.value(float(x));
    return t;
}

void heights() {
    const TerrainData t = slope();
    check(near(terrainHeight(t, 10.0f, 3.0f), 10.0f), "at a point");
    check(near(terrainHeight(t, 10.5f, 3.25f), 10.5f), "between points");
    check(near(terrainHeight(t, 64.0f, 30.5f), 64.0f), "mid-edge");
    check(near(terrainHeight(t, -5.0f, 10.0f), 0.0f), "outside the low edge clamps");
    check(near(terrainHeight(t, 100.0f, 200.0f), 64.0f), "outside the high edge clamps");
    const glm::vec3 n = terrainNormal(t, 20u, 20u);
    check(
        near(n.x, -std::sqrt(0.5f), 1e-3f) && near(n.y, std::sqrt(0.5f), 1e-3f) && near(n.z, 0.0f),
        "a 45 degree slope's normal");
    // A ray straight down finds the surface.
    float hit = 0.0f;
    check(
        raycastTerrain(t, {30.25f, 100.0f, 12.0f}, {0, -1, 0}, 500.0f, hit) && near(100.0f - hit, 30.25f, 0.01f),
        "a ray down hits the surface");
    check(!raycastTerrain(t, {-30.0f, 100.0f, 12.0f}, {0, -1, 0}, 500.0f, hit), "a ray outside misses");
}

void chunks() {
    TerrainData big = flatTerrain(513, 1.0f, 0.0f, 10.0f, 2.0f);
    check(terrainChunkCount(big) == 64, "513 samples make 64 chunks");
    const TerrainData t = slope();
    check(terrainChunkCount(t) == 1, "65 samples make one chunk");
    const MeshData mesh = buildTerrainChunk(t, 0);
    check(mesh.lods.size() == 2, "two simpler levels");
    // 64x64 quads, two triangles each, and a skirt of 256 edge segments, two each side.
    check(mesh.indices.size() == size_t(64 * 64 * 6 + 256 * 12), "full level triangles");
    check(mesh.lods[0].indexCount == uint32_t(32 * 32 * 6 + 128 * 12), "half level triangles");
    check(mesh.lods[1].indexCount == uint32_t(16 * 16 * 6 + 64 * 12), "quarter level triangles");
    check(mesh.lods[0].error < mesh.lods[1].error, "levels coarsen");
    bool inRange = true;
    for (uint32_t i : mesh.indices) inRange = inRange && i < mesh.vertices.size();
    for (uint32_t i : mesh.lodIndices) inRange = inRange && i < mesh.vertices.size();
    check(inRange, "indices name real vertices");
    // The skirt drops 4 * spacing below the edge.
    float lowest = 1e9f;
    for (const Vertex& v : mesh.vertices) lowest = std::min(lowest, v.position.y);
    check(near(lowest, -4.0f), "the skirt drops 4 spacings");
    // Upward faces: the first triangle's normal points up.
    const glm::vec3 a = mesh.vertices[mesh.indices[0]].position, b = mesh.vertices[mesh.indices[1]].position,
                    c = mesh.vertices[mesh.indices[2]].position;
    check(glm::cross(b - a, c - a).y > 0.0f, "triangles face up");
    check(near(mesh.vertices[5].uv.x, 5.0f / t.uvMetres), "uv is metres / uv_metres");
    // A change touches the chunks around it, with the border normals read.
    const auto touched = terrainChunksTouching(big, {64, 10, 64, 10});
    check(touched.size() == 2 && touched[0] == 0 && touched[1] == 1, "a point on a seam touches both chunks");
    check(terrainChunksTouching(big, {200, 200, 201, 201}).size() == 1, "a middle point touches one chunk");
    check(terrainChunksTouching(big, {1, 1, 0, 0}).empty(), "an empty rect touches none");

    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;
    terrainTriangles(t, 2, vertices, indices);
    check(
        vertices.size() == 33 * 33 && indices.size() == size_t(32 * 32 * 6), "navigation triangles at every 2nd point");
}

void brushes() {
    TerrainData t = flatTerrain(129, 1.0f, 0.0f, 100.0f, 10.0f);
    const TerrainData before = t;
    TerrainBrushStroke stroke;
    stroke.brush = TerrainBrush::Raise;
    stroke.centre = {40.0f, 50.0f};
    stroke.radius = 8.0f;
    stroke.strength = 1.0f;
    stroke.dt = 0.5f;
    glm::ivec4 rect{1, 1, 0, 0};
    for (int i = 0; i < 4; ++i) rect = unionRect(rect, applyTerrainBrush(t, stroke));
    bool outsideSame = true, insideRaised = true;
    for (uint32_t z = 0; z < t.samples; ++z)
        for (uint32_t x = 0; x < t.samples; ++x) {
            const float d = glm::length(glm::vec2(float(x), float(z)) - stroke.centre);
            const size_t i = z * t.samples + x;
            if (d >= stroke.radius) outsideSame = outsideSame && t.heights[i] == before.heights[i];
            if (d < stroke.radius * 0.5f) insideRaised = insideRaised && t.heights[i] > before.heights[i];
        }
    check(outsideSame, "a stroke changes only heights inside its radius");
    check(insideRaised, "a raise stroke raises the middle");
    check(rect.x >= 32 && rect.z <= 48 && rect.y >= 42 && rect.w <= 58, "the changed rect stays inside the circle");

    // Undo puts the bytes back exactly; redo brings the stroke back.
    const TerrainEdit edit = makeTerrainEdit("terrain/t.swterrain", before, t, rect);
    TerrainData undone = t;
    applyTerrainEdit(undone, edit, false);
    check(undone.heights == before.heights, "undo restores the bytes exactly");
    applyTerrainEdit(undone, edit, true);
    check(undone.heights == t.heights, "redo restores the stroke");

    // Flatten pulls towards a height; Smooth evens a spike; Lower lowers.
    TerrainData f = slope();
    TerrainBrushStroke flatten;
    flatten.brush = TerrainBrush::Flatten;
    flatten.centre = {32.0f, 32.0f};
    flatten.radius = 6.0f;
    flatten.strength = 1.0f;
    flatten.dt = 1.0f;
    flatten.flattenTo = 32.0f;
    for (int i = 0; i < 20; ++i) applyTerrainBrush(f, flatten);
    check(near(f.at(30, 32), 32.0f, 0.2f) && near(f.at(34, 32), 32.0f, 0.2f), "flatten levels the middle");
    TerrainData spike = flatTerrain(65, 1.0f, 0.0f, 100.0f, 10.0f);
    spike.heights[32 * 65 + 32] = spike.value(50.0f);
    TerrainBrushStroke smooth;
    smooth.brush = TerrainBrush::Smooth;
    smooth.centre = {32.0f, 32.0f};
    smooth.radius = 4.0f;
    smooth.strength = 1.0f;
    smooth.dt = 0.1f;
    applyTerrainBrush(spike, smooth);
    check(spike.at(32, 32) < 50.0f, "smooth lowers a spike");
    TerrainData low = flatTerrain(65, 1.0f, 0.0f, 100.0f, 10.0f);
    TerrainBrushStroke lower = smooth;
    lower.brush = TerrainBrush::Lower;
    applyTerrainBrush(low, lower);
    check(low.at(32, 32) < 10.0f, "lower lowers");
}

void files() {
    const fs::path root = fs::temp_directory_path() / "shockwave_terrain_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path content = root / "Content";
    TerrainData t = slope();
    t.heightsFile = "terrain/slope.r16";
    t.material = "materials/grass.swmaterial";
    std::string error;
    check(writeTerrain(content / "terrain/slope.swterrain", content, t, error), "writes: " + error);
    check(!fs::exists(content / "terrain/slope.r16.saving"), "no temporary file left");
    check(fs::file_size(content / "terrain/slope.r16") == 65u * 65u * 2u, "heights file size");
    TerrainData back;
    check(readTerrain(content / "terrain/slope.swterrain", content, back, error), "reads: " + error);
    check(back.heights == t.heights && back.samples == t.samples, "heights round-trip");
    // A heights file of the wrong size names both sizes.
    {
        std::ofstream out(content / "terrain/slope.r16", std::ios::binary | std::ios::trunc);
        out << "abcd";
    }
    check(
        !readTerrain(content / "terrain/slope.swterrain", content, back, error) &&
            error.find("has 4 bytes") != std::string::npos,
        "a short heights file is refused: " + error);
    fs::remove(content / "terrain/slope.r16", ec);
    check(
        !readTerrain(content / "terrain/slope.swterrain", content, back, error) &&
            error.find("missing") != std::string::npos,
        "a missing heights file is refused: " + error);
    // Importing a raw .r16.
    {
        std::ofstream out(root / "in.r16", std::ios::binary);
        for (uint32_t i = 0; i < 129u * 129u; ++i) {
            const uint16_t v = uint16_t(i % 65536);
            out.put(char(v & 0xff));
            out.put(char(v >> 8));
        }
    }
    uint32_t samples = 0;
    std::vector<uint16_t> values;
    check(
        importTerrainHeights(root / "in.r16", samples, values, error) && samples == 129 && values[300] == 300,
        "a .r16 imports: " + error);
    {
        std::ofstream out(root / "odd.r16", std::ios::binary);
        out << std::string(100 * 100 * 2, '\0');
    }
    check(
        !importTerrainHeights(root / "odd.r16", samples, values, error) &&
            error.find("100 by 100") != std::string::npos,
        "a .r16 of a size terrains can't be is refused: " + error);
    fs::remove_all(root, ec);
}

}   // namespace

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "all";
    if (which == "all" || which == "format") format();
    if (which == "all" || which == "heights") heights();
    if (which == "all" || which == "chunks") chunks();
    if (which == "all" || which == "brushes") brushes();
    if (which == "all" || which == "files") files();
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
