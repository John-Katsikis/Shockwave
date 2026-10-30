#pragma once

// Motion vectors, history and temporal anti-aliasing (G09, G10; docs/GraphicsImplementation.md
// 13 and 14, docs/TemporalAntiAliasing.md). The CPU side of every rule the GPU follows:
// Shaders/mesh.vert and prepass.frag write motion, Shaders/taa.comp resolves. The tests check
// these against independently worked values.
//
// Jitter J is in screen pixels, positive right and down, centred Halton(2, 3) over eight
// frames. With the scene's negative-height viewport and Y-up projections it's applied in
// clip space as
//   clip.x += ( 2 J.x / width)  clip.w
//   clip.y += (-2 J.y / height) clip.w
// which moves everything J / extent in screen UV (Renderer/ScreenSpace.h).
//
// Motion is stored unjittered: motion = currentUv - previousUv, both as the unjittered
// projections would put the surface. So where the current sample at uv was last frame, in
// the previous frame's (jittered) image, is
//   previousHistoryUv = uv - motion + previousJitter / previousExtent - currentJitter / currentExtent
// and history is reset whenever the extent changes, so the two extents are always equal.

#include <Renderer/RenderTypes.h>

#include <glm/glm.hpp>

#include <cstdint>

namespace shockwave {

// ---- Jitter ----

inline constexpr uint32_t kTaaJitterCycle = 8;   // frames before the pattern repeats
// The radical inverse of index in base (Halton): halton(1, 2) = 0.5, halton(2, 2) = 0.25...
float halton(uint32_t index, uint32_t base);
// Frame n's jitter in pixels, each axis in -0.5..0.5: Halton(2, 3) from index 1, centred.
glm::vec2 taaJitter(uint32_t frame);
// projection, moved by jitter pixels on a render target of extent pixels, as above.
glm::mat4 jitterProjection(const glm::mat4& projection, const glm::vec2& jitter, const glm::vec2& extent);

// ---- Motion and reprojection ----

inline glm::vec2 motionVector(const glm::vec2& currentUv, const glm::vec2& previousUv) { return currentUv - previousUv; }
inline glm::vec2 previousHistoryUv(
    const glm::vec2& uv, const glm::vec2& motion, const glm::vec2& previousJitter, const glm::vec2& previousExtent,
    const glm::vec2& currentJitter, const glm::vec2& currentExtent) {
    return uv - motion + previousJitter / previousExtent - currentJitter / currentExtent;
}

// ---- When history is thrown away ----

// Why a view's history was reset this frame; None: it carries on.
enum class HistoryReset : uint32_t {
    None,
    FirstFrame,   // nothing before it (a new renderer, or temporal effects just turned on)
    ViewChanged,   // another RenderViewId: one view's history never feeds another
    Resized,   // render extent (window size or render scale)
    ProjectionChanged,   // perspective to orthographic or back
    CameraCut,   // Renderer::cutCamera: a teleport or a cut the engine was told about
    ModeChanged,   // anti-aliasing or debug view changed, or a frame went by without TAA
};
const char* historyResetName(HistoryReset reset);   // "none", "first frame", ...

// What decides whether last frame's history still fits this one.
struct TemporalViewState {
    bool valid = false;   // a frame was drawn with temporal effects on
    RenderViewId view;
    glm::uvec2 extent{0};
    bool orthographic = false;
};
// cut: the engine asked for one. Checked in the order the enum lists them.
HistoryReset historyReset(const TemporalViewState& previous, const TemporalViewState& current, bool cut);

// ---- Resolve (Shaders/taa.comp) ----

// History is blended up to this much at rest, down towards kTaaMotionWeight once the surface
// moves kTaaMotionPixels or more a frame, and towards 0 where the reactive mask says
// something transparent changed it.
inline constexpr float kTaaRestWeight = 0.9f;
inline constexpr float kTaaMotionWeight = 0.7f;
inline constexpr float kTaaMotionPixels = 4.0f;
// Neighbourhood clipping: mean +- this many standard deviations, in YCoCg.
inline constexpr float kTaaVarianceScale = 1.0f;
float taaHistoryWeight(float motionPixels, float reactive);
// History is rejected when the depth it was drawn at is further than this from where the
// surface was expected last frame (metres, positive view distance).
inline float taaDepthTolerance(float expectedDistance) { return glm::max(0.01f, 0.01f * expectedDistance); }

glm::vec3 rgbToYCoCg(const glm::vec3& rgb);
glm::vec3 yCoCgToRgb(const glm::vec3& yCoCg);
// Moves history towards the box's centre until it's inside [lo, hi] (along the line to the
// centre, not per axis, so the colour's hue is kept); inside already: unchanged.
glm::vec3 clipToBox(const glm::vec3& history, const glm::vec3& lo, const glm::vec3& hi);

}   // namespace shockwave
