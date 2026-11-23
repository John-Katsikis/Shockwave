#pragma once
// Screen-space reflections (V1, docs/Reflections.md): a ray from each surface along its
// mirror direction through the surface prepass's depth; where it hits something on screen,
// the reflection is that spot of last frame's scene. Shaders/ssr.comp is the same code in
// GLSL; the screen_reflections test checks this one.
#include <glm/glm.hpp>

#include <cstdint>
#include <span>

namespace shockwave {

inline constexpr float kReflectionMaxDistance = 30.0f;   // metres of ray
inline constexpr float kReflectionThickness = 0.5f;   // metres a surface is assumed to be deep
inline constexpr float kReflectionStepGrowth = 1.05f;   // each step 5% longer than the last
inline constexpr uint32_t kReflectionRefinements = 4;   // bisection steps after a hit
inline constexpr float kReflectionStartOffset = 0.02f;   // metres along the normal before the first step
inline constexpr float kReflectionRoughnessFadeStart = 0.35f;   // rougher than this fades to the probes
inline constexpr float kReflectionRoughnessFadeEnd = 0.6f;
inline constexpr uint32_t kReflectionMips = 5;   // the result and four 2x2 box mips for rough surfaces

struct ReflectionHit {
    bool hit = false;
    glm::vec2 uv{0.0f};   // screen uv of what's reflected (this frame's)
    float confidence = 0.0f;   // 0..1: how much the reflection replaces the probes'
    float distance = 0.0f;   // metres along the ray
};

// The steps' first length, so steps of growth kReflectionStepGrowth add up to maxDistance.
float reflectionFirstStep(uint32_t steps, float maxDistance);

// From a reverse-Z depth buffer (row 0 at the top) of w x h texels, at pixel's centre with
// its view-space normal. Sky, rays towards the camera (view z > 0) and rays that leave the
// screen don't hit. A hit is the first step behind the depth buffer's surface by less than
// thicknessMetres, refined by kReflectionRefinements bisections. Confidence fades out over
// the last 20% of maxDistance, the outer 10% of the screen, and at grazing angles.
ReflectionHit traceReflection(
    std::span<const float> depth, uint32_t w, uint32_t h, glm::uvec2 pixel, const glm::mat4& projection,
    const glm::mat4& inverseProjection, glm::vec3 normalView, uint32_t steps,
    float maxDistanceMetres = kReflectionMaxDistance, float thicknessMetres = kReflectionThickness);

// How much of the screen-space reflection a surface of this perceptual roughness takes.
float reflectionRoughnessWeight(float roughness);

}   // namespace shockwave
