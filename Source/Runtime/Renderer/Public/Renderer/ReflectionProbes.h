#pragma once

// Local reflection probes (G07, docs/Lighting.md "Reflection probes"): the contract between
// the engine, which loads captured cubes and hands their boxes to the renderer, and the
// renderer, which picks up to two of them per draw and blends them per pixel over the
// global environment. Shaders/reflection.glsl is the same code in GLSL; the tests check
// the two agree on the cases written here.
//
// A probe is a box (its own rigid frame: rotation and position, no scale, half extents in
// metres) with a cube map captured from a point inside it. The cube's faces follow the
// world axes, not the box's, in the same orientation as the global environment's cubes
// (cubeFaceDirection), and its levels are the global specular cube's roughness steps:
// level L holds roughness L / (levels - 1).

#include <Renderer/RenderTypes.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace shockwave {

// The most probes a scene can hand the renderer at once, and the most a draw uses.
inline constexpr uint32_t kMaxReflectionProbes = 64;
inline constexpr uint32_t kMaxReflectionProbesPerDraw = 2;
// Mip levels of every captured cube: roughness 0, 0.2 .. 1, as the global specular cube.
inline constexpr uint32_t kReflectionLevels = 6;

struct ReflectionProbeDesc {
    glm::mat4 boxToWorld{1.0f};   // rotation and translation only
    glm::vec3 halfExtents{1.0f};   // metres, along the box's own axes; all positive
    glm::vec3 capturePosition{0.0f};   // world space, where the cube was captured from
    float blendDistance = 0.5f;   // metres inside the faces over which the probe fades out
    int32_t priority = 0;   // higher wins where boxes overlap
    uint64_t order = 0;   // stable tie-break among equals (the probe's binding id), lower first
    TextureHandle cube;   // a TextureShape::Cube texture with kReflectionLevels levels
};

// One frame's HDR scene colour read back (Renderer::requestSceneCapture): linear, before
// exposure, bloom, anti-aliasing and UI. rgba is width x height x 4 floats, rows top to
// bottom; the image is the camera's whole view however the target's aspect stretches it,
// so pixel (x, y) is NDC ((x + 0.5) / width * 2 - 1, 1 - (y + 0.5) / height * 2).
struct SceneCapture {
    uint32_t width = 0, height = 0;
    std::vector<float> rgba;
    std::string error;   // why there's no image, when there isn't
};

// ---- Cube faces ----

// The direction a cube texel looks along: face 0..5 is +X, -X, +Y, -Y, +Z, -Z, and
// st = (texel + 0.5) / size * 2 - 1 with t growing down the face. Unnormalised. The same
// as cubeDirection in Shaders/environment_filter.comp, so local and global cubes agree.
inline glm::vec3 cubeFaceDirection(uint32_t face, const glm::vec2& st) {
    switch (face) {
    case 0: return {1.0f, -st.y, -st.x};
    case 1: return {-1.0f, -st.y, st.x};
    case 2: return {st.x, 1.0f, st.y};
    case 3: return {st.x, -1.0f, -st.y};
    case 4: return {st.x, -st.y, 1.0f};
    default: return {-st.x, -st.y, -1.0f};
    }
}
// The inverse: which face a direction lands on and where (st in -1..1). Ties go to the
// first axis of x, y, z with the largest magnitude. A zero direction gives face 0, (0, 0).
inline uint32_t cubeFaceOf(const glm::vec3& d, glm::vec2& st) {
    const glm::vec3 a = glm::abs(d);
    if (a.x >= a.y && a.x >= a.z && a.x > 0.0f) {
        st = d.x > 0.0f ? glm::vec2(-d.z, -d.y) / a.x : glm::vec2(d.z, -d.y) / a.x;
        return d.x > 0.0f ? 0u : 1u;
    }
    if (a.y >= a.z && a.y > 0.0f) {
        st = d.y > 0.0f ? glm::vec2(d.x, d.z) / a.y : glm::vec2(d.x, -d.z) / a.y;
        return d.y > 0.0f ? 2u : 3u;
    }
    if (a.z > 0.0f) {
        st = d.z > 0.0f ? glm::vec2(d.x, -d.y) / a.z : glm::vec2(-d.x, -d.y) / a.z;
        return d.z > 0.0f ? 4u : 5u;
    }
    st = glm::vec2(0.0f);
    return 0;
}

// ---- Box projection ----

// Where in the probe's cube to look for a reflection ray from point (world space) along
// direction R: the ray is followed to where it leaves the box (slab method, in the box's
// own frame, with R left unnormalised so the parameter means the same in both frames),
// and the cube is read in the direction from the capture point to that exit. False when
// the point isn't inside the box, R is (near) zero, or no exit ahead is found: the caller
// then leaves the probe out (its weight is 0) and never reads a NaN.
inline bool
reflectionBoxProject(const ReflectionProbeDesc& probe, const glm::vec3& point, const glm::vec3& R, glm::vec3& lookup) {
    const glm::mat4 toBox = glm::inverse(probe.boxToWorld);
    const glm::vec3 p = glm::vec3(toBox * glm::vec4(point, 1.0f));
    const glm::vec3 r = glm::mat3(toBox) * R;
    const glm::vec3 h = probe.halfExtents;
    if (glm::any(glm::greaterThan(glm::abs(p), h))) return false;
    float exit = std::numeric_limits<float>::infinity();
    for (int i = 0; i < 3; ++i) {
        if (std::abs(r[i]) < 1e-8f) continue;   // parallel to this pair of faces: never leaves through them
        exit = std::min(exit, ((r[i] > 0.0f ? h[i] : -h[i]) - p[i]) / r[i]);
    }
    if (!(exit >= 0.0f) || !std::isfinite(exit)) return false;
    const glm::vec3 d = point + exit * R - probe.capturePosition;
    lookup = glm::dot(d, d) > 1e-12f ? d : R;
    return std::isfinite(lookup.x) && std::isfinite(lookup.y) && std::isfinite(lookup.z);
}

// How much of the probe a point gets, 0..1: 1 deeper inside than blendDistance from every
// face, falling linearly to 0 at the faces, 0 outside. A zero blendDistance is a hard edge.
inline float reflectionProbeWeight(const ReflectionProbeDesc& probe, const glm::vec3& point) {
    const glm::vec3 p = glm::vec3(glm::inverse(probe.boxToWorld) * glm::vec4(point, 1.0f));
    const glm::vec3 inside = probe.halfExtents - glm::abs(p);
    const float depth = std::min({inside.x, inside.y, inside.z});
    if (!(depth >= 0.0f)) return 0.0f;
    if (!(probe.blendDistance > 0.0f)) return 1.0f;
    return std::clamp(depth / probe.blendDistance, 0.0f, 1.0f);
}

// ---- Which probes a draw uses ----

// Up to kMaxReflectionProbesPerDraw probes whose boxes touch a bounding sphere (centre,
// radius): the highest priority first, then the smaller box, then the lower order. A large
// object that crosses more boxes than that keeps only those two; pixels outside both get
// the global environment.
struct ReflectionSelection {
    uint32_t count = 0;
    std::array<uint32_t, kMaxReflectionProbesPerDraw> index{};   // into the probes given
};
inline bool reflectionBoxTouchesSphere(const ReflectionProbeDesc& probe, const glm::vec4& sphere) {
    const glm::vec3 c = glm::vec3(glm::inverse(probe.boxToWorld) * glm::vec4(glm::vec3(sphere), 1.0f));
    const glm::vec3 nearest = glm::clamp(c, -probe.halfExtents, probe.halfExtents);
    const glm::vec3 gap = c - nearest;
    return glm::dot(gap, gap) <= sphere.w * sphere.w;
}
inline bool reflectionProbeBefore(const ReflectionProbeDesc& a, const ReflectionProbeDesc& b) {
    if (a.priority != b.priority) return a.priority > b.priority;
    const float va = a.halfExtents.x * a.halfExtents.y * a.halfExtents.z;
    const float vb = b.halfExtents.x * b.halfExtents.y * b.halfExtents.z;
    if (va != vb) return va < vb;
    return a.order < b.order;
}
inline ReflectionSelection
selectReflectionProbes(std::span<const ReflectionProbeDesc> probes, const glm::vec4& sphere) {
    ReflectionSelection s;
    for (uint32_t i = 0; i < probes.size(); ++i) {
        if (!reflectionBoxTouchesSphere(probes[i], sphere)) continue;
        // Insertion into the (at most two) best so far.
        uint32_t at = s.count;
        while (at > 0 && reflectionProbeBefore(probes[i], probes[s.index[at - 1]])) --at;
        if (at >= kMaxReflectionProbesPerDraw) continue;
        for (uint32_t j = std::min(s.count, kMaxReflectionProbesPerDraw - 1); j > at; --j) s.index[j] = s.index[j - 1];
        s.index[at] = i;
        s.count = std::min(s.count + 1, kMaxReflectionProbesPerDraw);
    }
    return s;
}

// ---- Blending per pixel ----

// What each selected probe contributes at a point, given their raw weights (weight[i] for
// selection.index[i], already 0 where box projection failed). Equal priorities share:
// local = sum(w s) / sum(w), coverage = min(sum(w), 1). A higher priority goes over a lower
// one: the first covers w0, the second w1 of what's left. Either way the result is
//   specular = sum(share[i] * probe i) + (1 - coverage) * global,   coverage = sum(share)
// and shares are never negative, never sum past 1.
struct ReflectionBlend {
    std::array<float, kMaxReflectionProbesPerDraw> share{};
    float coverage = 0.0f;
};
inline ReflectionBlend blendReflectionProbes(
    std::span<const ReflectionProbeDesc> probes, const ReflectionSelection& selection,
    const std::array<float, kMaxReflectionProbesPerDraw>& weight) {
    ReflectionBlend b;
    if (selection.count == 0) return b;
    const float w0 = std::clamp(weight[0], 0.0f, 1.0f);
    if (selection.count == 1) {
        b.share[0] = w0;
    } else {
        const float w1 = std::clamp(weight[1], 0.0f, 1.0f);
        if (probes[selection.index[0]].priority == probes[selection.index[1]].priority) {
            const float sum = w0 + w1;
            if (sum > 0.0f) {
                const float scale = std::min(sum, 1.0f) / sum;
                b.share = {w0 * scale, w1 * scale};
            }
        } else {
            b.share = {w0, w1 * (1.0f - w0)};
        }
    }
    b.coverage = std::min(b.share[0] + b.share[1], 1.0f);
    return b;
}

}   // namespace shockwave
