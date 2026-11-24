#pragma once
// Contact shadows (V4, docs/Shadows.md): a short ray from each pixel towards the sun
// through the surface prepass's depth, for the small shadows where things touch the ground
// that shadow maps are too coarse to show. Shaders/contact_shadows.comp is the same code in
// GLSL; the contact_shadows test checks this one.
#include <glm/glm.hpp>

#include <cstdint>
#include <span>

namespace shockwave {

inline constexpr float kContactShadowLength = 0.35f;   // metres of ray
inline constexpr uint32_t kContactShadowSteps = 12;
inline constexpr float kContactShadowThickness = 0.25f;   // metres a surface is assumed to be deep
inline constexpr float kContactShadowStartOffset = 0.02f;   // metres towards the sun before the first step
inline constexpr float kContactShadowBias = 0.01f;   // metres a surface must be in front of the ray

// The noise the scene's shadow filter uses (Shaders/mesh.glsl), 0..1 at a pixel centre.
float interleavedGradientNoise(glm::vec2 pixelCentre);

// 1 lit .. 0 blocked at pixel, from a reverse-Z depth buffer (row 0 at the top) of w x h
// texels: depth 0 (sky) is lit. sunDirectionView points towards the sun in view space.
// The ray starts kContactShadowStartOffset towards the sun, takes steps equal steps over
// lengthMetres (the first offset by the pixel's noise times a step), and is blocked at the
// first step where the depth buffer's surface is more than kContactShadowBias and less than
// thicknessMetres in front of it. A step off screen ends it lit.
float contactShadow(
    std::span<const float> depth, uint32_t w, uint32_t h, glm::uvec2 pixel, const glm::mat4& projection,
    const glm::mat4& inverseProjection, glm::vec3 sunDirectionView, float lengthMetres = kContactShadowLength,
    uint32_t steps = kContactShadowSteps, float thicknessMetres = kContactShadowThickness);

}   // namespace shockwave
