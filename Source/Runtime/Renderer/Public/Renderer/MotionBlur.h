#pragma once
// Motion blur (V3, docs/PostProcess.md): each pixel smeared along its motion since last
// frame, as a camera with a 1/120 s shutter would, scaled by the player's motion_blur.
// Shaders/motion_blur.comp is the same code in GLSL; the motion_blur test checks this one.
#include <glm/glm.hpp>

namespace shockwave {

inline constexpr float kMotionBlurShutter = 1.0f / 120.0f;   // seconds, at strength 1
inline constexpr float kMotionBlurMaxPixels = 32.0f;
inline constexpr float kMotionBlurMinPixels = 0.5f;   // shorter blurs nothing
inline constexpr unsigned kMotionBlurSamples = 8;

// The blur in pixels for a motion of motionUv (screen uv this frame minus last frame's):
// motion * strength * shutter / frameSeconds, at most kMotionBlurMaxPixels long, zero under
// kMotionBlurMinPixels or for a frame time that isn't positive.
glm::vec2 blurVector(glm::vec2 motionUv, float strength, float frameSeconds, glm::vec2 renderSize);

}   // namespace shockwave
