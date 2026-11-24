// Screen effects' shared maths (V1-V4): the CPU twins of contact_shadows.comp, ssr.comp,
// motion_blur.comp and dof.comp, and the settings' names.
#include <Renderer/ContactShadows.h>
#include <Renderer/DepthOfField.h>
#include <Renderer/MotionBlur.h>
#include <Renderer/ScreenEffects.h>
#include <Renderer/ScreenReflections.h>
#include <Renderer/ScreenSpace.h>

#include <algorithm>
#include <cmath>

namespace shockwave {

namespace {

float smooth(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The depth buffer's surface at uv, in view space; false for sky.
bool surfaceAt(
    std::span<const float> depth, uint32_t w, uint32_t h, glm::vec2 uv, const glm::mat4& inverseProjection,
    glm::vec3& surface) {
    const uint32_t x = std::min(static_cast<uint32_t>(std::max(uv.x * float(w), 0.0f)), w - 1);
    const uint32_t y = std::min(static_cast<uint32_t>(std::max(uv.y * float(h), 0.0f)), h - 1);
    const glm::vec2 centre((float(x) + 0.5f) / float(w), (float(y) + 0.5f) / float(h));
    return viewPositionFromDepth(centre, depth[size_t(y) * w + x], inverseProjection, surface);
}

// Where a view-space point lands on screen; false behind the camera or off screen.
bool project(const glm::mat4& projection, glm::vec3 point, glm::vec2& uv) {
    const glm::vec4 clip = projection * glm::vec4(point, 1.0f);
    if (!(clip.w > 1e-6f)) return false;   // behind the camera (orthographic w is always 1)
    uv = screenUvFromClip(clip);
    return uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f;
}

}   // namespace

const char* contactShadowsName(ContactShadows value) { return value == ContactShadows::On ? "on" : "off"; }

const char* screenReflectionsName(ScreenReflections quality) {
    switch (quality) {
    case ScreenReflections::Off: return "off";
    case ScreenReflections::Low: return "low";
    case ScreenReflections::Medium: return "medium";
    case ScreenReflections::High: return "high";
    }
    return "off";
}

uint32_t screenReflectionSteps(ScreenReflections quality) {
    switch (quality) {
    case ScreenReflections::Off: return 0;
    case ScreenReflections::Low: return 16;
    case ScreenReflections::Medium: return 32;
    case ScreenReflections::High: return 64;
    }
    return 0;
}

float interleavedGradientNoise(glm::vec2 p) {
    const float inner = glm::fract(glm::dot(p, glm::vec2(0.06711056f, 0.00583715f)));
    return glm::fract(52.9829189f * inner);
}

float contactShadow(
    std::span<const float> depth, uint32_t w, uint32_t h, glm::uvec2 pixel, const glm::mat4& projection,
    const glm::mat4& inverseProjection, glm::vec3 sunDirectionView, float lengthMetres, uint32_t steps,
    float thicknessMetres) {
    if (w == 0 || h == 0 || pixel.x >= w || pixel.y >= h || depth.size() < size_t(w) * h || steps == 0) return 1.0f;
    const glm::vec2 centre = glm::vec2(pixel) + 0.5f;
    glm::vec3 position;
    if (!viewPositionFromDepth(
            centre / glm::vec2(w, h), depth[size_t(pixel.y) * w + pixel.x], inverseProjection, position))
        return 1.0f;   // sky
    const float length = glm::length(sunDirectionView);
    if (!(length > 1e-6f)) return 1.0f;
    const glm::vec3 toSun = sunDirectionView / length;
    const glm::vec3 start = position + toSun * kContactShadowStartOffset;
    const float step = lengthMetres / float(steps);
    const float noise = interleavedGradientNoise(centre);
    for (uint32_t i = 0; i < steps; ++i) {
        const glm::vec3 point = start + toSun * ((float(i) + noise) * step);
        glm::vec2 uv;
        if (!project(projection, point, uv)) return 1.0f;
        glm::vec3 surface;
        if (!surfaceAt(depth, w, h, uv, inverseProjection, surface)) continue;
        const float gap = surface.z - point.z;   // positive: the surface is nearer the camera
        if (gap > kContactShadowBias && gap < thicknessMetres) return 0.0f;
    }
    return 1.0f;
}

float reflectionFirstStep(uint32_t steps, float maxDistance) {
    if (steps == 0) return maxDistance;
    const float growth = kReflectionStepGrowth;
    return maxDistance * (growth - 1.0f) / (std::pow(growth, float(steps)) - 1.0f);
}

ReflectionHit traceReflection(
    std::span<const float> depth, uint32_t w, uint32_t h, glm::uvec2 pixel, const glm::mat4& projection,
    const glm::mat4& inverseProjection, glm::vec3 normalView, uint32_t steps, float maxDistanceMetres,
    float thicknessMetres) {
    ReflectionHit none;
    if (w == 0 || h == 0 || pixel.x >= w || pixel.y >= h || depth.size() < size_t(w) * h || steps == 0 ||
        !(maxDistanceMetres > 0.0f))
        return none;
    const glm::vec2 centre = (glm::vec2(pixel) + 0.5f) / glm::vec2(w, h);
    glm::vec3 position;
    if (!viewPositionFromDepth(centre, depth[size_t(pixel.y) * w + pixel.x], inverseProjection, position))
        return none;   // sky
    const bool orthographic = std::abs(projection[2][3]) < 1e-6f;
    const glm::vec3 viewDir = orthographic ? glm::vec3(0, 0, -1) : glm::normalize(position);
    const float nLength = glm::length(normalView);
    if (!(nLength > 1e-6f)) return none;
    const glm::vec3 n = normalView / nLength;
    const glm::vec3 ray = glm::reflect(viewDir, n);
    if (ray.z > 0.0f) return none;   // towards the camera: what it sees isn't on screen
    const glm::vec3 origin = position + n * kReflectionStartOffset;
    float step = reflectionFirstStep(steps, maxDistanceMetres);
    float t = 0.0f;
    for (uint32_t i = 0; i < steps; ++i) {
        const float previous = t;
        t += step;
        step *= kReflectionStepGrowth;
        glm::vec2 uv;
        if (!project(projection, origin + ray * t, uv)) return none;
        glm::vec3 surface;
        if (!surfaceAt(depth, w, h, uv, inverseProjection, surface)) continue;
        const float gap = surface.z - (origin + ray * t).z;   // positive: the ray is behind it
        if (!(gap > 0.0f && gap < thicknessMetres)) continue;
        float lo = previous, hi = t;
        for (uint32_t k = 0; k < kReflectionRefinements; ++k) {
            const float mid = 0.5f * (lo + hi);
            const glm::vec3 point = origin + ray * mid;
            glm::vec2 m;
            glm::vec3 s;
            if (project(projection, point, m) && surfaceAt(depth, w, h, m, inverseProjection, s) &&
                s.z - point.z > 0.0f)
                hi = mid;
            else
                lo = mid;
        }
        ReflectionHit hit;
        hit.hit = true;
        hit.distance = hi;
        project(projection, origin + ray * hi, hit.uv);
        const float edge = std::min(std::min(hit.uv.x, 1.0f - hit.uv.x), std::min(hit.uv.y, 1.0f - hit.uv.y));
        hit.confidence = (1.0f - smooth(0.8f, 1.0f, hi / maxDistanceMetres)) * smooth(0.0f, 0.1f, edge) *
                         smooth(0.0f, 0.2f, std::max(glm::dot(-viewDir, n), 0.0f));
        return hit;
    }
    return none;
}

float reflectionRoughnessWeight(float roughness) {
    return 1.0f - smooth(kReflectionRoughnessFadeStart, kReflectionRoughnessFadeEnd, roughness);
}

glm::vec2 blurVector(glm::vec2 motionUv, float strength, float frameSeconds, glm::vec2 renderSize) {
    if (!(frameSeconds > 0.0f) || !(strength > 0.0f) || !std::isfinite(motionUv.x) || !std::isfinite(motionUv.y))
        return glm::vec2(0.0f);
    glm::vec2 pixels = motionUv * renderSize * (strength * kMotionBlurShutter / frameSeconds);
    const float length = glm::length(pixels);
    if (!(length >= kMotionBlurMinPixels)) return glm::vec2(0.0f);
    if (length > kMotionBlurMaxPixels) pixels *= kMotionBlurMaxPixels / length;
    return pixels;
}

float circleOfConfusion(
    float viewDistance, float focusDistance, float focusRange, float maxRadiusPixels, float falloffMetres) {
    if (!std::isfinite(viewDistance)) return maxRadiusPixels;   // the sky: behind everything
    const float offset = viewDistance - focusDistance;
    const float outside = std::abs(offset) - focusRange;
    if (outside <= 0.0f) return 0.0f;
    const float radius = std::min(outside / std::max(falloffMetres, 1e-4f), 1.0f) * maxRadiusPixels;
    return offset < 0.0f ? -radius : radius;
}

float dofFalloff(float focusDistance) { return std::max(focusDistance, 1.0f); }

float dofRadiusPixels(float dofBlur, uint32_t renderHeight) {
    return std::max(dofBlur, 0.0f) * float(renderHeight) / kDofReferenceHeight;
}

}   // namespace shockwave
