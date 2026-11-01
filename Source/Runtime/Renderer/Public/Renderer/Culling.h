#pragma once

// Batching, occlusion culling and indirect drawing (G12, docs/GraphicsImplementation.md
// section 16, docs/Drawing.md). None of it changes a pixel: instancing merges draws that
// were already the same mesh with the same pipeline, and occlusion culling only skips what
// the depth prepass proves hidden. The maths here is what shaders/hzb.comp and
// shaders/cull.comp do; tests check it by hand, and the GPU tests check the pictures match.

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace shockwave {

// ---- Settings (GraphicsSettings: instancing, occlusion_culling, pipeline_cache) ----
// Occlusion culling needs the surface prepass's depth: it runs only on frames that already
// draw the prepass (ambient occlusion, TAA or decals on), and never adds one.
enum class OcclusionCulling : uint32_t { Off, On };
const char* occlusionCullingName(OcclusionCulling culling);   // "off", "on"
struct DrawSettings {
    bool instancing = true;   // off: one draw call per object, as before G12
    OcclusionCulling occlusion = OcclusionCulling::On;
    bool operator==(const DrawSettings&) const = default;
};

// What the last frame drew and how (the graphics report's "drawing" block).
struct DrawStats {
    uint32_t objects = 0;   // draws in every pass, one per object a pass draws
    uint32_t calls = 0;   // draw calls recorded for them, indirect ones included
    uint32_t instanced = 0;   // objects that shared a call with the one before
    // Occlusion culling: whether it ran, why not, and how. tested and occluded are read
    // from an earlier frame's GPU counters, without waiting for them.
    bool occlusionActive = false;
    const char* occlusionReason = "";   // why it didn't run; empty when it did
    bool multiDrawIndirect = false;   // one call per group of draws; else one per draw
    uint32_t tested = 0;
    uint32_t occluded = 0;
    uint64_t hzbBytes = 0;
    // The pipeline cache: where it's kept, how much was loaded at start, and its state
    // ("off", "loaded", "new", "replaced: the driver or shaders changed", ...).
    std::string pipelineCacheFile;
    uint64_t pipelineCacheBytes = 0;
    const char* pipelineCacheState = "off";
};

// ---- Hierarchical depth (reverse-Z: 1 near, 0 far and cleared) ----
// Each level holds the minimum (farthest) depth of the texels it covers, so a test against
// it is conservative: a sky texel (0) under an object's rectangle keeps it visible.
// Level 0 halves the depth image (rounding up); each further level halves the one before,
// down to 1 x 1. A texel at level L covers depth pixels [i 2^(L+1), (i + 1) 2^(L+1)).
// The GPU pads level 0 to a power of two a side with 1.0 (nearest), which reads the same.
struct HzbLevel {
    uint32_t width = 0, height = 0;
    std::vector<float> depth;
};
std::vector<HzbLevel> buildHzb(const std::vector<float>& depth, uint32_t width, uint32_t height);

// An object's bounding box on screen: depth-image pixels (top-left origin), and the depth
// of its nearest corner. valid false means it can't be judged (it crosses the near plane
// or wraps behind the camera): it's drawn.
struct ScreenRect {
    bool valid = false;
    glm::vec2 min{0.0f}, max{0.0f};
    float nearestDepth = 0.0f;
};
// The eight corners of the sphere's bounding cube, through viewProjection (the jittered
// one the prepass drew with), into a width x height image, widened a pixel each way.
ScreenRect projectBounds(glm::vec4 sphere, const glm::mat4& viewProjection, uint32_t width, uint32_t height);
// The level whose texels the rectangle spans at most 4 of a side (so at most 5 x 5 are read).
uint32_t hzbLevelFor(const ScreenRect& rect, uint32_t levels);
// True only when every texel under the rectangle at that level is nearer than the object's
// nearest corner by more than bias. Rectangles past the screen's edge are clamped to it.
inline constexpr float kHzbBias = 1e-6f;
bool hzbOccluded(const std::vector<HzbLevel>& hzb, const ScreenRect& rect, float bias = kHzbBias);

}   // namespace shockwave
