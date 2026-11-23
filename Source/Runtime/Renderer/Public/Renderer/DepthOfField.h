#pragma once
// Depth of field (V2, docs/PostProcess.md): what's nearer or farther than a post-process
// profile's focus blurs, by a circle of confusion in pixels. Shaders/dof.comp is the same
// code in GLSL; the depth_of_field test checks this one.
#include <cstdint>

namespace shockwave {

inline constexpr float kDofReferenceHeight = 1080.0f;   // dof_blur is pixels at this many lines
inline constexpr uint32_t kDofGatherSamples = 22;   // 1 + 7 + 14 in three rings

// Signed blur radius in pixels: 0 within focusRange of focusDistance, rising linearly to
// maxRadiusPixels over falloffMetres beyond that, negative in front of the focus and
// positive behind it. A view distance that isn't finite (the sky) is infinitely far.
float circleOfConfusion(
    float viewDistance, float focusDistance, float focusRange, float maxRadiusPixels, float falloffMetres);

// The falloff the renderer uses: max(focusDistance, 1) metres.
float dofFalloff(float focusDistance);

// dof_blur scaled to a render of renderHeight lines.
float dofRadiusPixels(float dofBlur, uint32_t renderHeight);

}   // namespace shockwave
