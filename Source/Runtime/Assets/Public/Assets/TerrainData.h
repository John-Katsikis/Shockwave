#pragma once

// Heightmap terrain (W1a, docs/Terrain.md).
//
// A .swterrain file is a small text header naming a heights file beside it:
//
//     swterrain 1
//     samples 513                     # points along each side: 65, 129, 257, 513, 1025 or 2049
//     spacing 1                       # metres between points
//     height_range 0 200              # metres: a height value of 0 is low, 65535 is high
//     heights "terrain/hills.r16"     # Content path: samples * samples little-endian uint16
//     material "materials/grass.swmaterial"
//     uv_metres 4                     # metres per repeat of the material's textures
//
// Version 2 (W1b) paints up to four layers instead of one material:
//
//     swterrain 2
//     ...samples, spacing, height_range, heights as above...
//     layer 0 "materials/grass.swmaterial" 4      # index, material, metres per texture repeat
//     layer 1 "materials/rock.swmaterial" 8
//     weights "terrain/hills.weights.png"         # RGBA8, samples by samples: layer i's share in channel i
//
// Each point's four shares are normalised to sum to 255 when read. The weights image's top
// row is the row at the lowest Z, like the heights.
//
// The heights file holds rows along +X, the first row at the lowest Z: the value for point
// (x, z) is at index z * samples + x. Height in metres = low + value / 65535 * (high - low).
// The terrain's corner with the lowest X and Z sits at its entity's position.

#include <Renderer/Mesh.h>
#include <Renderer/RenderTypes.h>
#include <Renderer/Shading.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

// A painted layer (version 2): kTerrainLayerCount slots, used or not.
struct TerrainLayer {
    bool used = false;
    std::string material;   // .swmaterial in Content; empty: the default surface
    float uvMetres = 4.0f;   // metres per repeat of its textures
    bool operator==(const TerrainLayer&) const = default;
};

struct TerrainData {
    uint32_t version = 1;   // 1: one material; 2: painted layers and weights
    uint32_t samples = 129;
    float spacing = 1.0f;
    float low = 0.0f, high = 100.0f;
    std::vector<uint16_t> heights;   // samples * samples
    std::string material;   // .swmaterial in Content; empty: the default surface
    float uvMetres = 4.0f;
    std::string heightsFile;   // Content path of the heights, as the header names it
    // Version 2: the layers and their weights, samples * samples * 4 bytes, each point's
    // shares summing to 255 (normaliseTerrainWeights).
    std::array<TerrainLayer, kTerrainLayerCount> layers{};
    std::string weightsFile;   // Content path of the weights PNG
    std::vector<uint8_t> weights;

    bool painted() const { return version >= 2; }
    float size() const { return float(samples - 1) * spacing; }   // metres along each side
    float metres(uint16_t value) const { return low + float(value) / 65535.0f * (high - low); }
    uint16_t value(float metres) const;   // clamped to the range
    float at(uint32_t x, uint32_t z) const { return metres(heights[size_t(z) * samples + x]); }
};

inline constexpr uint32_t kTerrainChunkQuads = 64;   // quads along each side of a render chunk
inline constexpr uint32_t kTerrainMinSamples = 65, kTerrainMaxSamples = 2049;
bool validTerrainSamples(uint32_t samples);   // 2^n + 1 within 65..2049
bool isTerrainFile(const std::filesystem::path& path);   // .swterrain
// A painted terrain's weights (name.weights.png): read by its terrain, never loaded as a texture.
bool isTerrainWeightsFile(const std::filesystem::path& path);

// The header alone (heights stay empty). Strict: an unknown key or version, a value out of
// range, a key given twice or a missing one fails with "line N: ...".
bool parseTerrain(std::string_view text, TerrainData& terrain, std::string& error);
std::string terrainText(const TerrainData& terrain);   // the header, canonical
// The header and its heights file (found in content). The heights file must be exactly
// samples * samples * 2 bytes.
bool readTerrain(
    const std::filesystem::path& file, const std::filesystem::path& content, TerrainData& terrain, std::string& error);
// Writes the header to file and the heights to content / heightsFile, each to a temporary
// name first and then renamed over the old one, so a reader never sees half a file.
bool writeTerrain(
    const std::filesystem::path& file, const std::filesystem::path& content, const TerrainData& terrain,
    std::string& error);
// A flat terrain at height (metres) with heightsFile named after file.
TerrainData flatTerrain(uint32_t samples, float spacing, float low, float high, float height);

// ---- Painted layers (W1b) ----
// The layers as drawn: version 2's, or version 1's material as layer 0.
std::array<TerrainLayer, kTerrainLayerCount> terrainLayers(const TerrainData& terrain);
uint32_t terrainLayerCount(const TerrainData& terrain);   // used layers
// Unused layers' shares become 0 and each point's shares sum to exactly 255, rounded so the
// largest remainders win. A point with no share at all goes to the first used layer.
void normaliseTerrainWeights(TerrainData& terrain);
// Adds a layer in the first free slot and returns its index (-1 with error when all four are
// used). A version 1 terrain becomes version 2: its material is layer 0, everywhere, and its
// weights file is named after the heights (hills.r16: hills.weights.png).
int addTerrainLayer(TerrainData& terrain, std::string material, float uvMetres, std::string& error);
// Removes a layer, sharing its weight among the others in proportion. The last one can't go.
bool removeTerrainLayer(TerrainData& terrain, uint32_t layer, std::string& error);
// The layers with any share above 1% (3 of 255) at the points chunk draws: bit i for layer i.
// Version 1: 1.
uint32_t terrainChunkLayers(const TerrainData& terrain, uint32_t chunk);

// Height in metres at (x, z) metres from the terrain's corner: bilinear between the points,
// clamped to the edge outside.
float terrainHeight(const TerrainData& terrain, float x, float z);
// Unit normal at a point, from central differences of the heights.
glm::vec3 terrainNormal(const TerrainData& terrain, uint32_t x, uint32_t z);
glm::vec3 terrainNormal(const TerrainData& terrain, float x, float z);
// Nearest hit of a ray (in the terrain's space) with the surface, marching in steps of
// spacing / 2 up to maxDistance and refining the crossing. t: distance along direction.
bool raycastTerrain(
    const TerrainData& terrain, const glm::vec3& origin, const glm::vec3& direction, float maxDistance, float& t);

// ---- Render chunks ----
uint32_t terrainChunksPerSide(const TerrainData& terrain);
uint32_t terrainChunkCount(const TerrainData& terrain);
// One chunk's mesh in the terrain's space: every point, with two simpler levels (every
// 2nd and every 4th point) as lods, and a skirt around each level dropping 4 * spacing.
// UV = position XZ / uvMetres. A level's error is set so it switches at about 150 m and
// 400 m from a 1080p camera with lod_error_pixels 1 (docs/Terrain.md).
MeshData buildTerrainChunk(const TerrainData& terrain, uint32_t chunk);
Bounds terrainChunkBounds(const TerrainData& terrain, uint32_t chunk);
// Chunks whose mesh reads any point in [x0, x1] x [z0, z1] (inclusive point indices),
// counting the one-point border normals read.
std::vector<uint32_t> terrainChunksTouching(const TerrainData& terrain, glm::ivec4 rect);

// Triangles for collision-free uses (navigation): two per quad, every step-th point.
void terrainTriangles(
    const TerrainData& terrain, uint32_t step, std::vector<glm::vec3>& vertices, std::vector<uint32_t>& indices);

// ---- Sculpting ----
// Paint (W1b) raises one layer's share and lowers the others in proportion.
enum class TerrainBrush { Raise, Lower, Smooth, Flatten, Paint };
enum class TerrainFalloff { Smooth, Linear };
struct TerrainBrushStroke {
    TerrainBrush brush = TerrainBrush::Raise;
    TerrainFalloff falloff = TerrainFalloff::Smooth;
    glm::vec2 centre{0.0f};   // metres from the corner, XZ
    float radius = 10.0f;   // metres, 1..200
    float strength = 0.5f;   // 0..1
    float dt = 1.0f / 60.0f;   // seconds this application covers
    float flattenTo = 0.0f;   // metres, for Flatten
    uint32_t layer = 0;   // for Paint
};
// Applies one frame of a brush; returns the changed points as (x0, z0, x1, z1), inclusive,
// or x0 > x1 when nothing changed. Only points within the radius change. Paint changes the
// weights (only on a painted terrain, with the layer used), the others the heights.
glm::ivec4 applyTerrainBrush(TerrainData& terrain, const TerrainBrushStroke& stroke);
// The heights inside rect (inclusive), row by row, and putting them back.
std::vector<uint16_t> terrainRegion(const TerrainData& terrain, glm::ivec4 rect);
void setTerrainRegion(TerrainData& terrain, glm::ivec4 rect, std::span<const uint16_t> values);
glm::ivec4 unionRect(glm::ivec4 a, glm::ivec4 b);   // an empty rect (x0 > x1) adds nothing
// The weights inside rect (inclusive), 4 bytes a point, and putting them back.
std::vector<uint8_t> terrainWeightsRegion(const TerrainData& terrain, glm::ivec4 rect);
void setTerrainWeightsRegion(TerrainData& terrain, glm::ivec4 rect, std::span<const uint8_t> values);
// What adding or removing a layer changes besides the weights.
struct TerrainLayout {
    uint32_t version = 1;
    std::array<TerrainLayer, kTerrainLayerCount> layers{};
    std::string weightsFile;
    bool operator==(const TerrainLayout&) const = default;
};
TerrainLayout terrainLayout(const TerrainData& terrain);
// One undo step for the editor: a stroke's rectangle, before and after: the heights, the
// weights (painted terrains), and the layout when layers were added or removed.
struct TerrainEdit {
    std::string asset;
    glm::ivec4 rect{1, 1, 0, 0};
    std::vector<uint16_t> before, after;
    std::vector<uint8_t> weightsBefore, weightsAfter;
    bool layout = false;
    TerrainLayout layoutBefore, layoutAfter;
    std::string materialBefore, materialAfter;   // with layout: version 1's material
    float uvMetresBefore = 4.0f, uvMetresAfter = 4.0f;
};
TerrainEdit makeTerrainEdit(std::string asset, const TerrainData& before, const TerrainData& after, glm::ivec4 rect);
// Puts the edit's before (undo) or after (redo) heights, weights and layout back.
void applyTerrainEdit(TerrainData& terrain, const TerrainEdit& edit, bool redo);

// ---- Importing ----
// A 16-bit greyscale PNG (8-bit PNGs are widened) or a raw .r16 of samples^2 values. The
// image must be square with a side terrain samples allow.
bool importTerrainHeights(
    const std::filesystem::path& file, uint32_t& samples, std::vector<uint16_t>& heights, std::string& error);

// ---- In the asset registry ----
struct TerrainChunk {
    MeshHandle mesh{};
    Bounds bounds;
    uint32_t layers = 1;   // terrainChunkLayers
};
struct TerrainAsset {
    std::shared_ptr<const TerrainData> data;
    TextureHandle weights{};   // painted terrains: the weights, RGBA8 linear, one texel per point
    std::vector<TerrainChunk> chunks;   // terrainChunkCount, row by row along +X
    uint64_t revision = 0;   // bumps with every change to the heights
};
// Uploads every chunk of data.
std::shared_ptr<TerrainAsset> uploadTerrain(class Renderer& renderer, std::shared_ptr<const TerrainData> data);
// The asset after data replaced previous's in rect: chunks that touch it get new meshes,
// the rest keep theirs. The replaced meshes are returned in retired, for the caller to destroy.
// changed says what: heights rebuild the touched chunks' meshes; weights (painting, layers
// added or removed) update the weights texture in place and work out the touched chunks'
// layers again. The weights texture carries over from previous whatever changed.
enum TerrainChange : uint32_t { kTerrainHeightsChanged = 1, kTerrainWeightsChanged = 2 };
std::shared_ptr<TerrainAsset> updateTerrain(
    class Renderer& renderer, const TerrainAsset& previous, std::shared_ptr<const TerrainData> data, glm::ivec4 rect,
    std::vector<MeshHandle>& retired, uint32_t changed = kTerrainHeightsChanged);
void releaseTerrain(class Renderer& renderer, const TerrainAsset& terrain);

}   // namespace shockwave
