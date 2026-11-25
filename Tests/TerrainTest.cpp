// Heightmap terrain data (W1a, W1b, docs/Terrain.md): the .swterrain format and its errors,
// heights between and beyond the points, render chunks and their levels, brushes and undo,
// importing raw heights, and painted layers (format 2, weights, Paint, undo). No GPU.
#include <Assets/Image.h>
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
        {"swterrain 3\n", "line 1: only swterrain versions 1 and 2"},
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

const char* kPainted = "swterrain 2\n"
                       "samples 65\n"
                       "spacing 1\n"
                       "height_range 0 10\n"
                       "heights \"terrain/p.r16\"\n"
                       "layer 0 \"materials/grass.swmaterial\" 4\n"
                       "layer 2 \"materials/rock.swmaterial\" 8\n"
                       "weights \"terrain/p.weights.png\"\n";

uint32_t sum(const TerrainData& t, size_t point) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < kTerrainLayerCount; ++i) total += t.weights[point * 4 + i];
    return total;
}

void layers() {
    std::string error;
    // Version 2 parses, round-trips, and its errors name their lines.
    TerrainData t;
    check(parseTerrain(kPainted, t, error), "version 2 parses: " + error);
    check(t.version == 2 && t.painted() && t.layers[0].used && !t.layers[1].used && t.layers[2].used, "layers read");
    check(t.layers[2].material == "materials/rock.swmaterial" && t.layers[2].uvMetres == 8.0f, "layer values");
    check(t.weightsFile == "terrain/p.weights.png" && terrainLayerCount(t) == 2, "weights and count");
    TerrainData again;
    check(parseTerrain(terrainText(t), again, error) && terrainText(again) == terrainText(t), "version 2 round-trips");
    const std::string head = "swterrain 2\nsamples 65\nspacing 1\nheight_range 0 1\nheights \"a.r16\"\n";
    struct Bad {
        std::string text, expect;
    };
    const Bad bad[] = {
        {head + "material \"m.swmaterial\"\n", "line 6: swterrain 2 paints layers"},
        {head + "uv_metres 2\n", "line 6: swterrain 2 gives each layer"},
        {head + "layer 4 \"m.swmaterial\" 4\n", "line 6: layer index must be 0 to 3"},
        {head + "layer 0 \"m.swmaterial\" 4\nlayer 0 \"n.swmaterial\" 4\n", "line 7: layer 0 given twice"},
        {head + "layer 0 \"m.png\" 4\n", "line 6: a layer's material must be"},
        {head + "layer 0 \"m.swmaterial\" 0\n", "line 6: a layer's metres per repeat"},
        {head + "layer 0 \"m.swmaterial\"\n", "line 6: layer needs an index"},
        {head + "layer 0 \"m.swmaterial\" 4\nweights \"w.r16\"\n", "line 7: weights must be a .png"},
        {head + "weights \"w.png\"\n", "swterrain 2 needs at least one layer line"},
        {head + "layer 0 \"m.swmaterial\" 4\n", "weights is missing"},
        {"swterrain 1\nlayer 0 \"m.swmaterial\" 4\n", "line 2: layer needs \"swterrain 2\""},
        {"swterrain 1\nweights \"w.png\"\n", "line 2: weights needs \"swterrain 2\""},
    };
    for (const Bad& b : bad) {
        TerrainData ignored;
        check(
            !parseTerrain(b.text, ignored, error) && error.find(b.expect) != std::string::npos,
            "\"" + b.text + "\" fails with \"" + b.expect + "\", got \"" + error + "\"");
    }

    // A version 1 file loads as one layer: its material.
    TerrainData one;
    check(parseTerrain(kGood, one, error), "version 1 parses");
    const auto drawn = terrainLayers(one);
    check(
        terrainLayerCount(one) == 1 && drawn[0].used && drawn[0].material == "materials/grass.swmaterial" &&
            drawn[0].uvMetres == 8.0f && !drawn[1].used,
        "a version 1 terrain is one layer");
    check(terrainChunkLayers(one, 0) == 1u, "a version 1 chunk uses layer 0");

    // Weights normalise: every point sums to 255, unused layers get nothing, an empty point
    // goes to the first used layer.
    TerrainData w = flatTerrain(65, 1.0f, 0.0f, 10.0f, 0.0f);
    w.version = 2;
    w.layers[0] = {true, "", 4.0f};
    w.layers[2] = {true, "", 4.0f};
    w.weights.assign(65u * 65u * 4u, 0);
    w.weights[0 * 4 + 0] = 10;   // 10 : 30, layer 1 unused
    w.weights[0 * 4 + 1] = 200;
    w.weights[0 * 4 + 2] = 30;
    w.weights[1 * 4 + 0] = 1;   // 1 : 1 : 1 : 1 over two used layers
    w.weights[1 * 4 + 1] = 1;
    w.weights[1 * 4 + 2] = 1;
    w.weights[1 * 4 + 3] = 1;
    w.weights[2 * 4 + 1] = 77;   // only an unused layer
    normaliseTerrainWeights(w);
    bool sums = true;
    for (size_t p = 0; p < 65u * 65u; ++p)
        sums &= sum(w, p) == 255 && w.weights[p * 4 + 1] == 0 && w.weights[p * 4 + 3] == 0;
    check(sums, "every point sums to 255, unused layers 0");
    check(
        w.weights[0] == 64 && w.weights[2] == 191,
        "10 : 30 becomes 64 : 191, got " + std::to_string(w.weights[0]) + " : " + std::to_string(w.weights[2]));
    check(w.weights[4] == 128 && w.weights[6] == 127, "a tie rounds up the lower layer");
    check(w.weights[8] == 255, "a point with no used share goes to the first used layer");
    check(w.weights[12] == 255, "an all-zero point goes to the first used layer");

    // Painting layer 2 at a point makes its share 255 after enough strokes, the others 0;
    // points outside the radius don't change.
    TerrainData p = w;
    for (size_t q = 0; q < 65u * 65u; ++q) {
        p.weights[q * 4 + 0] = 255;
        p.weights[q * 4 + 2] = 0;
    }
    const TerrainData unpainted = p;
    TerrainBrushStroke paint;
    paint.brush = TerrainBrush::Paint;
    paint.layer = 2;
    paint.centre = {32.0f, 32.0f};
    paint.radius = 5.0f;
    paint.strength = 0.5f;
    glm::ivec4 touched{1, 1, 0, 0};
    for (int frame = 0; frame < 600; ++frame) touched = unionRect(touched, applyTerrainBrush(p, paint));
    const size_t centre = 32u * 65u + 32u;
    check(
        p.weights[centre * 4 + 2] == 255 && p.weights[centre * 4 + 0] == 0,
        "painting gets the layer to 255, got " + std::to_string(p.weights[centre * 4 + 2]));
    bool outsideSame = true, allSum = true;
    for (uint32_t z = 0; z < 65; ++z)
        for (uint32_t x = 0; x < 65; ++x) {
            const size_t q = size_t(z) * 65 + x;
            allSum &= sum(p, q) == 255;
            if (glm::length(glm::vec2(float(x), float(z)) - paint.centre) >= paint.radius)
                outsideSame &= std::equal(&p.weights[q * 4], &p.weights[q * 4] + 4, &unpainted.weights[q * 4]);
        }
    check(outsideSame, "painting changes nothing outside the radius");
    check(allSum, "painting keeps every point at 255");
    check(touched == glm::ivec4(28, 28, 36, 36), "the changed rectangle is the circle's points");
    check(p.heights == unpainted.heights, "painting leaves the heights");
    // A layer that isn't used can't be painted; nor can a version 1 terrain.
    TerrainBrushStroke unused = paint;
    unused.layer = 1;
    check(applyTerrainBrush(p, unused).x > applyTerrainBrush(p, unused).z, "an unused layer paints nothing");
    TerrainData v1 = flatTerrain(65, 1.0f, 0.0f, 10.0f, 0.0f);
    check(applyTerrainBrush(v1, paint).x > 0, "a version 1 terrain paints nothing");
    // Chunk layers: the painted chunk uses layers 0 and 2... this terrain is one chunk.
    check(terrainChunkLayers(p, 0) == 0b101u, "the chunk uses layers 0 and 2");
    check(terrainChunkLayers(unpainted, 0) == 0b001u, "an unpainted chunk uses layer 0 only");

    // Undo puts the weights back exactly, redo the painted ones.
    const TerrainEdit edit = makeTerrainEdit("t", unpainted, p, touched);
    TerrainData undone = p;
    applyTerrainEdit(undone, edit, false);
    check(undone.weights == unpainted.weights, "undo restores the weights' bytes");
    applyTerrainEdit(undone, edit, true);
    check(undone.weights == p.weights, "redo restores the painted bytes");

    // Adding a layer to a version 1 terrain makes it version 2 with its material as layer 0;
    // removing a layer shares its weight out; the last one stays.
    TerrainData grow = flatTerrain(65, 1.0f, 0.0f, 10.0f, 0.0f);
    grow.heightsFile = "terrain/grow.r16";
    grow.material = "materials/grass.swmaterial";
    grow.uvMetres = 6.0f;
    const TerrainData grown = grow;
    check(addTerrainLayer(grow, "materials/rock.swmaterial", 3.0f, error) == 1, "the new layer is 1: " + error);
    check(
        grow.painted() && grow.layers[0].material == "materials/grass.swmaterial" && grow.layers[0].uvMetres == 6.0f &&
            grow.material.empty() && grow.weightsFile == "terrain/grow.weights.png",
        "version 1 became version 2: " + grow.weightsFile);
    check(grow.weights.size() == 65u * 65u * 4u && grow.weights[0] == 255 && grow.weights[1] == 0, "all layer 0");
    const TerrainEdit added = makeTerrainEdit("t", grown, grow, {1, 1, 0, 0});
    TerrainData back = grow;
    applyTerrainEdit(back, added, false);
    check(
        !back.painted() && back.material == "materials/grass.swmaterial" && back.uvMetres == 6.0f,
        "undoing the first layer added gives version 1 back");
    applyTerrainEdit(back, added, true);
    check(back.painted() && back.weights == grow.weights && back.layers == grow.layers, "and redo");
    check(addTerrainLayer(grow, "", 4.0f, error) == 2 && addTerrainLayer(grow, "", 4.0f, error) == 3, "up to four");
    check(addTerrainLayer(grow, "", 4.0f, error) < 0 && error.find("at most 4") != std::string::npos, "no fifth");
    for (size_t q = 0; q < 65u * 65u; ++q) {
        grow.weights[q * 4 + 0] = 55;
        grow.weights[q * 4 + 1] = 100;
        grow.weights[q * 4 + 2] = 50;
        grow.weights[q * 4 + 3] = 50;
    }
    check(removeTerrainLayer(grow, 1, error), "removes layer 1: " + error);
    check(
        !grow.layers[1].used && grow.weights[1] == 0 && sum(grow, 0) == 255 && grow.weights[0] == 91 &&
            grow.weights[2] == 82 && grow.weights[3] == 82,
        "its share goes to the others in proportion, got " + std::to_string(grow.weights[0]) + " " +
            std::to_string(grow.weights[2]) + " " + std::to_string(grow.weights[3]));
    check(removeTerrainLayer(grow, 2, error) && removeTerrainLayer(grow, 3, error), "down to one");
    check(!removeTerrainLayer(grow, 0, error) && error.find("at least one") != std::string::npos, "the last stays");

    // Files: the weights PNG round-trips; one of the wrong size, or missing, is refused.
    const fs::path root = fs::temp_directory_path() / "shockwave_terrain_layers_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path content = root / "Content";
    p.heightsFile = "terrain/p.r16";
    p.weightsFile = "terrain/p.weights.png";
    check(writeTerrain(content / "terrain/p.swterrain", content, p, error), "writes a painted terrain: " + error);
    TerrainData read;
    check(readTerrain(content / "terrain/p.swterrain", content, read, error), "reads it: " + error);
    check(read.painted() && read.weights == p.weights && read.layers == p.layers, "weights and layers round-trip");
    check(
        !isTerrainWeightsFile("terrain/p.png") && isTerrainWeightsFile("terrain/p.weights.png"), "weights files known");
    {
        const std::vector<uint8_t> small(4u * 4u * 4u, 255);
        const std::vector<uint8_t> png = encodePngRgba8(4, 4, small);
        std::ofstream out(content / "terrain/p.weights.png", std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    }
    check(
        !readTerrain(content / "terrain/p.swterrain", content, read, error) &&
            error.find("is 4 by 4; 65 samples need 65 by 65") != std::string::npos,
        "weights of the wrong size are refused: " + error);
    fs::remove(content / "terrain/p.weights.png", ec);
    check(
        !readTerrain(content / "terrain/p.swterrain", content, read, error) &&
            error.find("weights file terrain/p.weights.png is missing") != std::string::npos,
        "missing weights are refused: " + error);
    // The PNG encoder's output decodes to the same pixels.
    std::vector<uint8_t> pixels(7u * 5u * 4u);
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = uint8_t(i * 37);
    Image decoded;
    check(
        decodeImage(encodePngRgba8(7, 5, pixels), decoded, error) && decoded.width == 7 && decoded.height == 5 &&
            decoded.rgba == pixels,
        "the PNG encoder round-trips: " + error);
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
    if (which == "all" || which == "layers") layers();
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
