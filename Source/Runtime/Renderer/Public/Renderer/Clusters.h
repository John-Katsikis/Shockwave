#pragma once

// Clustered local lighting (G11; docs/GraphicsImplementation.md 15, docs/ClusteredLighting.md).
// The CPU side of the rules Shaders/clusters.comp builds lists with and Shaders/mesh.frag
// looks them up by. Clustering only changes which point and spot lights a pixel tests:
// their maths, shadows and colour are the light loop's, and the brute-force loop over every
// light stays as the reference (light_culling brute_force).
//
// The view is cut into tiles of tileSize x tileSize render pixels, and each tile into
// slices by positive view distance z, logarithmically between the camera's near plane and
// the scene's cluster_far:
//   slice = clamp(floor(log(z / near) / log(far / near) * slices), 0, slices - 1)
// A cluster's list holds every local light whose sphere (position, range) touches the
// cluster's whole volume, so transparent surfaces anywhere in it find their lights too.
// Past cluster_far, in a cluster whose list overflowed, and in orthographic views or
// captures, a pixel loops over every local light instead: a light is never left out.
// Directional lights aren't clustered: every pixel tests them first, as a short list.

#include <glm/glm.hpp>

#include <cstdint>

namespace shockwave {

// How local lights are found per pixel (the light_culling setting).
//   Auto        clustered when the frame has at least kClusterAutoLights local lights,
//               brute force below (where building the lists costs more than it saves)
//   Clustered   always, where clusters apply
//   BruteForce  every pixel tests every light: no cluster pass, no buffers
enum class LightCulling : uint32_t { Auto, Clustered, BruteForce };
const char* lightCullingName(LightCulling culling);   // "auto", "clustered", "brute_force"

inline constexpr uint32_t kClusterTileSize = 32;   // render pixels, before the memory budget
inline constexpr uint32_t kClusterSlices = 16;
inline constexpr uint32_t kClusterCapacity = 64;   // lights a cluster lists; more overflows
// Each list packs four light indices a word (the light table has at most 256 lights), after
// one word of count: the count's top bit is the overflow flag.
inline constexpr uint32_t kClusterWords = 1 + kClusterCapacity / 4;
inline constexpr uint32_t kClusterOverflow = 0x80000000u;
// The most the lists may take; the tile size doubles until they fit.
inline constexpr uint64_t kClusterBudgetBytes = 16ull << 20;
inline constexpr uint32_t kClusterAutoLights = 8;   // local lights, measured: docs/ClusteredLighting.md
inline constexpr float kDefaultClusterFar = 250.0f;   // metres (Environment::cluster_far)

struct ClusterGrid {
    uint32_t tileSize = 0;   // pixels
    uint32_t tilesX = 0, tilesY = 0, slices = 0;
    uint32_t clusters() const { return tilesX * tilesY * slices; }
    uint64_t bytes() const { return uint64_t(clusters()) * kClusterWords * 4; }
};
// The grid for a render extent: kClusterTileSize tiles, doubled until the lists fit the budget.
ClusterGrid clusterGrid(uint32_t width, uint32_t height, uint64_t budgetBytes = kClusterBudgetBytes);

// Positive view distance z's slice, as above; z >= far is past the clusters (brute force).
uint32_t clusterSlice(float z, float nearPlane, float farPlane, uint32_t slices);
// Where slice s starts (s = slices: far).
float clusterSliceStart(uint32_t s, float nearPlane, float farPlane, uint32_t slices);

struct ClusterBounds {
    glm::vec3 lo, hi;   // view space (camera looking down -Z)
};
// The view-space box around cluster (x, y, s) of a perspective view: its tile grown by
// one pixel each way (TAA's jitter moves samples up to half a pixel), between its slice's
// distances grown by 0.1%. inverseProjection is the projection's as rasterized.
ClusterBounds clusterBounds(
    const ClusterGrid& grid, uint32_t x, uint32_t y, uint32_t s, const glm::vec2& renderSize,
    const glm::mat4& inverseProjection, float nearPlane, float farPlane);
// Whether a light's sphere touches the box; ranges are grown by 0.1% the same way.
bool sphereTouchesBox(const glm::vec3& centre, float radius, const ClusterBounds& box);
// The near plane distance of a reverse-Z projection: where depth is 1. 0 if it isn't a
// perspective projection (orthographic views don't cluster).
float projectionNearPlane(const glm::mat4& projection);

// Last frame's clusters (Renderer::clusterStats).
struct ClusterStats {
    LightCulling requested = LightCulling::Auto;
    bool clustered = false;   // lists were built and read
    const char* reason = "";   // why not, when they weren't
    ClusterGrid grid;
    float farPlane = 0;   // cluster_far used
    uint32_t localLights = 0;   // point and spot lights in the frame's table
    uint32_t directionalLights = 0;
    uint64_t listBytes = 0;   // both frames in flight
    // From the GPU, a frame or two late (read once its frame finished):
    uint32_t maxCandidates = 0;   // lights in the fullest cluster (before capping)
    float averageCandidates = 0;   // per cluster
    uint32_t overflowClusters = 0;   // clusters past kClusterCapacity (brute force there)
};

}   // namespace shockwave
