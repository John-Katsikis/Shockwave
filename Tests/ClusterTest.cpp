// Clustered local lighting (G11; docs/ClusteredLighting.md): the grid and its memory budget,
// logarithmic depth slices at the near and far extremes, cluster boxes that hold every pixel
// of their tile (with TAA's jitter) at every distance of their slice, the light sphere test,
// the near plane of reverse-Z projections, and the light_culling setting. No GPU: these run
// anywhere. The expected values are worked out by hand here.

#include <Engine/Camera.h>
#include <Engine/GraphicsSettings.h>
#include <Renderer/Clusters.h>
#include <Renderer/ScreenSpace.h>
#include <Renderer/Temporal.h>

#include <cmath>
#include <cstdio>
#include <string>

using namespace shockwave;

namespace {
int failures = 0;
void check(bool value, const std::string& message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}
bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps * std::max(1.0f, std::abs(b)); }
// Reverse-Z, infinite far plane: device depth at positive view distance z (Camera.cpp).
float infiniteDepth(float z, float nearPlane) { return nearPlane / z; }
bool inside(const glm::vec3& p, const ClusterBounds& box) {
    return glm::all(glm::greaterThanEqual(p, box.lo)) && glm::all(glm::lessThanEqual(p, box.hi));
}

void testGrid() {
    // The spec's example: 1920x1080 at 32 pixels is 60 x 34 x 16 = 32640 clusters, 17 words each.
    ClusterGrid g = clusterGrid(1920, 1080);
    check(g.tileSize == 32 && g.tilesX == 60 && g.tilesY == 34 && g.slices == 16, "1080p grid");
    check(g.clusters() == 32640 && g.bytes() == 32640ull * 17 * 4, "1080p cluster count and bytes");
    // 4K fits the 16 MiB budget at 32 pixels (8.5 MiB); a tighter budget doubles the tile size.
    g = clusterGrid(3840, 2160);
    check(g.tileSize == 32 && g.tilesX == 120 && g.tilesY == 68, "4K grid within the budget");
    g = clusterGrid(3840, 2160, 4ull << 20);
    check(g.tileSize == 64 && g.tilesX == 60 && g.tilesY == 34 && g.bytes() <= (4ull << 20), "4K grid over budget");
    // Tiny targets still get one tile.
    g = clusterGrid(10, 7);
    check(g.tilesX == 1 && g.tilesY == 1, "a target smaller than a tile");
    check(kClusterCapacity % 4 == 0 && kClusterWords == 17 && kMaxLights <= 256, "list packing fits the light table");
}

void testSlices() {
    const float n = 0.1f, f = 250.0f;
    check(clusterSlice(n, n, f, 16) == 0, "the near plane is slice 0");
    check(clusterSlice(0.01f, n, f, 16) == 0, "nearer than the near plane clamps to slice 0");
    check(clusterSlice(f * 0.9999f, n, f, 16) == 15, "just inside far is the last slice");
    check(clusterSlice(f * 10.0f, n, f, 16) == 15, "past far clamps (the shader tests every light there)");
    // Logarithmic: slice s starts at n (f/n)^(s/16); the middle slice starts at sqrt(n f).
    check(near(clusterSliceStart(8, n, f, 16), std::sqrt(n * f)), "slice 8 starts at sqrt(near far)");
    check(near(clusterSliceStart(16, n, f, 16), f) && near(clusterSliceStart(0, n, f, 16), n), "slice ends");
    for (uint32_t s = 0; s < 16; ++s) {
        const float a = clusterSliceStart(s, n, f, 16), b = clusterSliceStart(s + 1, n, f, 16);
        check(clusterSlice(a * 1.0001f, n, f, 16) == s, "a slice's start is in it: " + std::to_string(s));
        check(clusterSlice(std::sqrt(a * b), n, f, 16) == s, "a slice's middle is in it: " + std::to_string(s));
    }
    // Extremes: a very near near plane and a far far plane still give 16 increasing slices.
    float previous = 0.0f;
    for (uint32_t s = 0; s <= 16; ++s) {
        const float z = clusterSliceStart(s, 0.001f, 100000.0f, 16);
        check(std::isfinite(z) && z > previous, "slices increase at extreme near/far: " + std::to_string(s));
        previous = z;
    }
}

// Every pixel of a tile, jittered by up to half a pixel, at every distance of its slice, lies
// in its cluster's box.
void testBounds() {
    const glm::vec2 size(1280, 720);
    const float n = 0.1f, f = 250.0f;
    const ClusterGrid grid = clusterGrid(1280, 720);
    for (const glm::vec2 jitter : {glm::vec2(0.0f), glm::vec2(0.5f, -0.5f), glm::vec2(-0.5f, 0.5f)}) {
        const glm::mat4 projection = jitterProjection(perspectiveReverseZ(1.0f, size.x / size.y, n), jitter, size);
        const glm::mat4 inverse = glm::inverse(projection);
        check(near(projectionNearPlane(projection), n), "the jittered projection's near plane");
        for (uint32_t x : {0u, 7u, grid.tilesX - 1}) {
            for (uint32_t y : {0u, 11u, grid.tilesY - 1}) {
                for (uint32_t s : {0u, 5u, 15u}) {
                    const ClusterBounds box = clusterBounds(grid, x, y, s, size, inverse, n, f);
                    const float z0 = clusterSliceStart(s, n, f, 16), z1 = clusterSliceStart(s + 1, n, f, 16);
                    for (float px : {0.0f, 0.5f, 31.5f, 32.0f}) {
                        for (float py : {0.0f, 16.0f, 32.0f}) {
                            // The pixel at (x * 32 + px, y * 32 + py), rasterized with jitter.
                            const glm::vec2 pixel = glm::vec2(x, y) * 32.0f + glm::vec2(px, py);
                            for (float z : {z0, std::sqrt(z0 * z1), z1}) {
                                const glm::vec2 uv = pixel / size;
                                const glm::vec4 h =
                                    inverse *
                                    glm::vec4(2.0f * uv.x - 1.0f, 1.0f - 2.0f * uv.y, infiniteDepth(z, n), 1.0f);
                                const glm::vec3 p = glm::vec3(h) / h.w;
                                check(
                                    inside(p, box), "pixel (" + std::to_string(pixel.x) + ", " +
                                                        std::to_string(pixel.y) + ") at " + std::to_string(z) +
                                                        " m lies in its cluster");
                            }
                        }
                    }
                }
            }
        }
    }
}

void testSphere() {
    const ClusterBounds box{glm::vec3(-1, -1, -10), glm::vec3(1, 1, -5)};
    check(sphereTouchesBox(glm::vec3(0, 0, -7), 0.1f, box), "a light inside the box");
    check(sphereTouchesBox(glm::vec3(3, 0, -7), 2.0f, box), "a light whose range just reaches the box");
    check(!sphereTouchesBox(glm::vec3(3.1f, 0, -7), 2.0f, box), "a light 0.1 m short of the box");
    // Corner: distance sqrt(2) from (2, 2, -7) to (1, 1, -7).
    check(sphereTouchesBox(glm::vec3(2, 2, -7), 1.4143f, box), "a light reaching the box's edge");
    check(!sphereTouchesBox(glm::vec3(2, 2, -7), 1.40f, box), "a light short of the box's edge");
}

void testNearPlane() {
    check(near(projectionNearPlane(perspectiveReverseZ(1.2f, 16.0f / 9.0f, 0.05f)), 0.05f), "near plane 0.05");
    check(near(projectionNearPlane(perspectiveReverseZ(0.5f, 1.0f, 3.0f)), 3.0f), "near plane 3");
    check(projectionNearPlane(orthographicReverseZ(10, 5, 0.1f, 100)) == 0.0f, "orthographic: no clusters");
}

void testSetting() {
    check(
        std::string(lightCullingName(LightCulling::Auto)) == "auto" &&
            std::string(lightCullingName(LightCulling::Clustered)) == "clustered" &&
            std::string(lightCullingName(LightCulling::BruteForce)) == "brute_force",
        "light culling names");
    GraphicsSettings s;
    check(s.lightCulling == LightCulling::Auto, "light_culling defaults to auto");
    const GraphicsSettingInfo* info = findGraphicsSetting("light_culling");
    check(info != nullptr, "light_culling is a graphics setting");
    if (!info) return;
    std::string error;
    check(info->set(s, "brute_force", error) && s.lightCulling == LightCulling::BruteForce, "set brute_force");
    check(info->get(s) == "brute_force", "brute_force reads back");
    check(!info->set(s, "tiled", error) && error.find("light_culling") != std::string::npos, "unknown values refused");
    // Not a preset key: every preset keeps the player's choice.
    s.lightCulling = LightCulling::Clustered;
    check(graphicsPreset(GraphicsPreset::Low, s).lightCulling == LightCulling::Clustered, "presets keep light_culling");
}
}   // namespace

int main() {
    testGrid();
    testSlices();
    testBounds();
    testSphere();
    testNearPlane();
    testSetting();
    if (failures) {
        std::fprintf(stderr, "%d cluster check(s) failed\n", failures);
        return 1;
    }
    std::printf("clusters: all checks passed\n");
    return 0;
}
