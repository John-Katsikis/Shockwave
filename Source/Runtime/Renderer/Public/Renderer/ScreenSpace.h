#pragma once

// The screen-space coordinate contract (G03, docs/GraphicsImplementation.md 7.2). Every
// pass that turns a pixel back into a position (ambient occlusion now; decals, motion
// vectors and fog later) uses these definitions, and Shaders/screen.glsl is the same code
// in GLSL. Tests check one against the other's expectations at every corner.
//
// Screen UV is image coordinates: u grows to the right, v grows downwards, (0, 0) is the
// top-left corner of the top-left pixel. Projections are the engine's unmodified Y-up,
// reverse-Z ones (depth 1 at the near plane, 0 at infinity or the far plane); the scene
// pass draws with a negative-height viewport, so this is also the image the depth buffer
// holds. Depth 0 is background (sky): nothing was drawn there, and it has no position.

#include <glm/glm.hpp>

#include <cmath>

namespace shockwave {

// clip = projection * view * vec4(world, 1). Undefined for clip.w <= 0 (behind the camera).
inline glm::vec2 screenUvFromClip(const glm::vec4& clip) {
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    return {0.5f * ndc.x + 0.5f, 0.5f - 0.5f * ndc.y};
}

// The view-space position at screen uv with device depth, through the inverse of the
// projection that produced that depth. False for background (depth <= 0) and for results
// that aren't finite; never divides by a zero w. Works for perspective and orthographic.
inline bool viewPositionFromDepth(
    const glm::vec2& uv, float depth, const glm::mat4& inverseProjection, glm::vec3& viewPosition) {
    if (!(depth > 0.0f)) return false;
    const glm::vec4 h = inverseProjection * glm::vec4(2.0f * uv.x - 1.0f, 1.0f - 2.0f * uv.y, depth, 1.0f);
    if (!(h.w > 1e-20f || h.w < -1e-20f)) return false;
    viewPosition = glm::vec3(h) / h.w;
    return glm::all(glm::lessThan(glm::abs(viewPosition), glm::vec3(1e30f)));
}

// For perspectiveReverseZ (infinite far plane) only: positive distance along the view
// direction is near / depth. Not valid for orthographic or finite projections; general
// code uses viewPositionFromDepth.
inline float infinitePerspectiveDistance(float depth, float nearPlane) { return nearPlane / depth; }

// Reverse-Z orthographic projection: depth 1 at nearPlane, 0 at farPlane, Y up, looking
// down -Z. For orthographic views (editor top/side views, tests).
inline glm::mat4 orthographicReverseZ(float halfWidth, float halfHeight, float nearPlane, float farPlane) {
    glm::mat4 m(1.0f);
    m[0][0] = 1.0f / halfWidth;
    m[1][1] = 1.0f / halfHeight;
    // z_view = -near -> 1, z_view = -far -> 0.
    m[2][2] = 1.0f / (farPlane - nearPlane);
    m[3][2] = farPlane / (farPlane - nearPlane);
    return m;
}

// Unit vectors in two numbers in -1..1 (octahedral mapping), as the surface prepass stores
// normals. octDecode(octEncode(n)) is n to within float rounding.
inline glm::vec2 octEncode(glm::vec3 n) {
    n /= std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    glm::vec2 e(n.x, n.y);
    if (n.z < 0.0f)
        e = (1.0f - glm::abs(glm::vec2(n.y, n.x))) * glm::vec2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
    return e;
}
inline glm::vec3 octDecode(glm::vec2 e) {
    glm::vec3 n(e.x, e.y, 1.0f - std::abs(e.x) - std::abs(e.y));
    if (n.z < 0.0f) {
        const glm::vec2 folded =
            (1.0f - glm::abs(glm::vec2(n.y, n.x))) * glm::vec2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
        n.x = folded.x;
        n.y = folded.y;
    }
    return glm::normalize(n);
}

}   // namespace shockwave
