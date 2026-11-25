#pragma once

// Foliage (W2a, docs/Foliage.md): tens of thousands of small meshes (grass, bushes, rocks,
// trees) drawn cheaply.
//
// A .swfoliage file is a small text header naming its types and an instances file beside it:
//
//     swfoliage 1
//     instances "foliage/meadow.instances"     # Content path: 24 bytes an instance
//     type "models/grass.glb"                  # type 0; the keys under it are its own
//       distance 60                            # metres: drawn up to here (times foliage_distance)
//       fade 15                                # metres before distance where they thin out
//       shadows false                          # cast shadows (default true)
//       wind 0.15                              # metres of sway per metre of height (default 0)
//     type "models/rock.glb"                   # type 1
//
// Each instance is 24 bytes, little-endian: uint16 type, uint16 unused (0), float x, y, z,
// float yaw (radians), float scale. Positions are relative to the entity.

#include <Renderer/Mesh.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

inline constexpr size_t kFoliageInstanceBytes = 24;
inline constexpr float kFoliageCellMetres = 16.0f;   // instances are grouped into cells this wide
inline constexpr size_t kFoliageMaxTypes = 256;

struct FoliageType {
    std::string model;   // .glb/.gltf/... in Content
    float distance = 80.0f;   // metres, 1..5000
    float fade = 10.0f;   // metres, 0..distance
    bool shadows = true;
    float wind = 0.0f;   // 0..1
    bool operator==(const FoliageType&) const = default;
};

struct FoliageInstance {
    uint16_t type = 0;
    glm::vec3 position{0.0f};   // metres from the entity
    float yaw = 0.0f;   // radians about +Y
    float scale = 1.0f;
    float random = 0.0f;   // fixed 0..1 from its index in the file (foliageRandom): density and fade
    bool operator==(const FoliageInstance&) const = default;
};

// One type's instances in one 16 m cell: instances[first, first + count).
struct FoliageCell {
    uint16_t type = 0;
    glm::ivec2 coord{0};   // floor(x / 16), floor(z / 16)
    uint32_t first = 0, count = 0;
    Bounds bounds;   // the instances' positions (the model's size is added when drawn)
    float maxScale = 0.0f;   // the largest instance scale, for that
};

struct FoliageData {
    std::vector<FoliageType> types;
    std::string instancesFile;   // Content path, as the header names it
    // In the file's order until sortFoliage; then by type, then cell.
    std::vector<FoliageInstance> instances;
    std::vector<FoliageCell> cells;   // sortFoliage
};

bool isFoliageFile(const std::filesystem::path& path);   // .swfoliage

// The header alone (instances stay empty). Strict: an unknown key or version, a value out of
// range, a key given twice (per type) or a missing one fails with "line N: ...".
bool parseFoliage(std::string_view text, FoliageData& foliage, std::string& error);
std::string foliageText(const FoliageData& foliage);   // the header, canonical
// The instances from their bytes: a count of bytes divisible by 24, every type below
// typeCount. random is set from each one's index.
bool decodeFoliageInstances(
    std::span<const uint8_t> bytes, size_t typeCount, std::vector<FoliageInstance>& instances, std::string& error);
std::vector<uint8_t> encodeFoliageInstances(std::span<const FoliageInstance> instances);
// The header and its instances file (found in content), sorted into cells.
bool readFoliage(
    const std::filesystem::path& file, const std::filesystem::path& content, FoliageData& foliage, std::string& error);
// Writes the instances, then the header, each to a temporary name renamed over the old one.
// Instances are written in the order they're in.
bool writeFoliage(
    const std::filesystem::path& file, const std::filesystem::path& content, const FoliageData& foliage,
    std::string& error);
// Sorts the instances by type, then 16 m cell, and builds the cells.
void sortFoliage(FoliageData& foliage);

// A fixed number 0..1 for the instance at index (a hash: the same every run).
float foliageRandom(uint32_t index);
// foliage_density: an instance is drawn when its number is below the density.
inline bool foliageDense(float random, float density) { return random < density; }
// Thinning out between distance - fade and distance: the share of instances dropped at
// this distance (0 nearer, 1 at the far end), and whether one survives it.
float foliageFade(float distance, float maxDistance, float fade);
inline bool foliageSurvivesFade(float random, float fraction) { return fraction <= 0.0f || random > fraction; }
// Where an instance's model goes: translate, turn about Y, scale.
glm::mat4 foliageMatrix(const FoliageInstance& instance);
// Metres from point to the nearest point of a cell's box (0 inside).
float foliageCellDistance(const FoliageCell& cell, const glm::vec3& point);

}   // namespace shockwave
