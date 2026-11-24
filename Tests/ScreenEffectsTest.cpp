// Screen effects' CPU maths (V1-V4; docs/Shadows.md, docs/Reflections.md, docs/PostProcess.md):
// the twins of contact_shadows.comp, ssr.comp, motion_blur.comp and dof.comp. Depth buffers
// are ray traced here from boxes and planes, through the renderer's reverse-Z projection.
// No GPU. Run with one of: contact_shadows, screen_reflections, motion_blur, depth_of_field.
#include <Renderer/ContactShadows.h>
#include <Renderer/DepthOfField.h>
#include <Renderer/MotionBlur.h>
#include <Renderer/ScreenEffects.h>
#include <Renderer/ScreenReflections.h>
#include <Renderer/ScreenSpace.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace shockwave;

namespace {
int failures = 0;
void check(bool value, const std::string& message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

// Reverse-Z with an infinite far plane, as the renderer draws (RendererFrame.cpp).
glm::mat4 reverseZ(float fovY, float aspect, float zNear) {
    const float f = 1.0f / std::tan(fovY * 0.5f);
    glm::mat4 m(0.0f);
    m[0][0] = f / aspect;
    m[1][1] = f;
    m[2][3] = -1.0f;
    m[3][2] = zNear;
    return m;
}

struct Box {
    glm::vec3 lo, hi;
};

// A camera and a world of boxes, ray traced into a depth buffer.
struct Scene {
    uint32_t w = 320, h = 180;
    glm::mat4 view{1.0f}, projection{1.0f}, inverseProjection{1.0f};
    std::vector<Box> boxes;
    std::vector<float> depth;

    Scene(glm::vec3 eye, glm::vec3 target) {
        view = glm::lookAt(eye, target, glm::vec3(0, 1, 0));
        projection = reverseZ(glm::radians(60.0f), float(w) / float(h), 0.05f);
        inverseProjection = glm::inverse(projection);
    }
    void trace() {
        depth.assign(size_t(w) * h, 0.0f);
        const glm::mat4 toWorld = glm::inverse(view);
        const glm::vec3 eye = glm::vec3(toWorld[3]);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const glm::vec2 uv((float(x) + 0.5f) / float(w), (float(y) + 0.5f) / float(h));
                glm::vec3 near(0.0f);
                viewPositionFromDepth(uv, 1.0f, inverseProjection, near);
                const glm::vec3 dir = glm::normalize(glm::vec3(toWorld * glm::vec4(near, 0.0f)));
                float best = std::numeric_limits<float>::infinity();
                for (const Box& b : boxes) {
                    float t0 = 0.0f, t1 = best;
                    for (int a = 0; a < 3; ++a) {
                        if (std::abs(dir[a]) < 1e-9f) {
                            if (eye[a] < b.lo[a] || eye[a] > b.hi[a]) t0 = t1 + 1.0f;
                            continue;
                        }
                        float ta = (b.lo[a] - eye[a]) / dir[a], tb = (b.hi[a] - eye[a]) / dir[a];
                        if (ta > tb) std::swap(ta, tb);
                        t0 = std::max(t0, ta);
                        t1 = std::min(t1, tb);
                    }
                    if (t0 <= t1 && t0 < best) best = t0;
                }
                if (!std::isfinite(best)) continue;
                const glm::vec4 clip = projection * view * glm::vec4(eye + dir * best, 1.0f);
                depth[size_t(y) * w + x] = clip.z / clip.w;
            }
    }
    // The pixel a world point lands on.
    glm::uvec2 pixelOf(glm::vec3 world) const {
        const glm::vec2 uv = screenUvFromClip(projection * view * glm::vec4(world, 1.0f));
        return glm::uvec2(glm::clamp(uv * glm::vec2(w, h), glm::vec2(0.0f), glm::vec2(w - 1, h - 1)));
    }
    glm::vec2 uvOf(glm::vec3 world) const { return screenUvFromClip(projection * view * glm::vec4(world, 1.0f)); }
    glm::vec3 toView(glm::vec3 direction) const { return glm::mat3(view) * direction; }
};

const Box kFloor{{-50, -1, -50}, {50, 0, 50}};

void testContactShadows() {
    const glm::vec3 toSun = glm::normalize(glm::vec3(1, 1, 0));   // 45 degrees up, from +x
    {
        Scene s({0, 1.5f, 3}, {0, 0, 0});
        s.boxes = {kFloor};
        s.trace();
        int shadowed = 0, floor = 0;
        for (uint32_t y = 0; y < s.h; ++y)
            for (uint32_t x = 0; x < s.w; ++x) {
                if (s.depth[size_t(y) * s.w + x] <= 0.0f) continue;
                ++floor;
                shadowed +=
                    contactShadow(s.depth, s.w, s.h, {x, y}, s.projection, s.inverseProjection, s.toView(toSun)) < 1.0f;
            }
        check(floor > 1000 && shadowed == 0, "a flat floor shadows itself nowhere (" + std::to_string(shadowed) + ")");
        check(s.depth[s.w / 2] == 0.0f, "the top row is sky");
        check(
            contactShadow(s.depth, s.w, s.h, {s.w / 2, 0}, s.projection, s.inverseProjection, s.toView(toSun)) == 1.0f,
            "the sky (depth 0) is lit");
    }
    Scene s({0, 0.8f, 1.5f}, {0, 0, 0});
    s.boxes = {kFloor, {{-0.05f, 0, -0.05f}, {0.05f, 0.1f, 0.05f}}, {{-5, 0, -2.1f}, {5, 3, -2}}};
    s.trace();
    auto at = [&](glm::vec3 world) {
        return contactShadow(s.depth, s.w, s.h, s.pixelOf(world), s.projection, s.inverseProjection, s.toView(toSun));
    };
    for (float x : {-0.07f, -0.1f, -0.13f})
        check(
            at({x, 0, 0}) == 0.0f,
            "the floor within 10 cm of a 10 cm cube, away from the sun, is in its contact shadow");
    for (float x : {-0.45f, -0.6f}) check(at({x, 0, 0}) == 1.0f, "the floor 40 cm past the cube is lit");
    check(at({0.2f, 0, 0}) == 1.0f, "the floor on the sun's side of the cube is lit");
    check(at({0.5f, 1.5f, -2}) == 1.0f && at({-1, 2, -2}) == 1.0f, "a wall 2 m behind is lit");
    check(
        interleavedGradientNoise({0.5f, 0.5f}) >= 0.0f && interleavedGradientNoise({100.5f, 7.5f}) < 1.0f,
        "noise is in 0..1");
}

void testScreenReflections() {
    const glm::vec3 eye{0, 1, 3};
    Scene s(eye, {0, 0.6f, -5});
    const Box wall{{-50, -1, -5.5f}, {50, 50, -5}};
    s.boxes = {kFloor, wall};
    s.trace();
    const glm::vec3 up = s.toView({0, 1, 0});
    const uint32_t steps = screenReflectionSteps(ScreenReflections::High);
    // Down the middle column, the mirror floor's reflections of the wall land where the
    // mirrored ray meets it, to within a pixel.
    int hits = 0, checked = 0;
    float nearestEdge = 1.0f, nearestConfidence = 1.0f;
    for (uint32_t y = s.h / 2; y < s.h; y += 3) {
        const glm::uvec2 pixel{s.w / 2, y};
        glm::vec3 position;
        const glm::vec2 uv((float(pixel.x) + 0.5f) / float(s.w), (float(y) + 0.5f) / float(s.h));
        if (!viewPositionFromDepth(uv, s.depth[size_t(y) * s.w + pixel.x], s.inverseProjection, position)) continue;
        const glm::vec3 world = glm::vec3(glm::inverse(s.view) * glm::vec4(position, 1.0f));
        if (world.y > 0.01f) continue;   // the wall
        const glm::vec3 d = glm::normalize(world - eye);
        const glm::vec3 r{d.x, -d.y, d.z};
        const glm::vec3 expected = world + r * ((-5.0f - world.z) / r.z);
        const glm::vec2 expectedUv = s.uvOf(expected);
        if (expectedUv.y < 0.02f) continue;   // fades out there anyway
        ++checked;
        const ReflectionHit hit =
            traceReflection(s.depth, s.w, s.h, pixel, s.projection, s.inverseProjection, up, steps);
        if (!hit.hit) continue;
        ++hits;
        const glm::vec2 error = (hit.uv - expectedUv) * glm::vec2(s.w, s.h);
        check(
            glm::length(error) <= 1.0f, "row " + std::to_string(y) + ": the reflection is " +
                                            std::to_string(glm::length(error)) + " px from the wall's mirror point");
        const float edge = std::min(std::min(hit.uv.x, 1 - hit.uv.x), std::min(hit.uv.y, 1 - hit.uv.y));
        check(hit.confidence >= 0.0f && hit.confidence <= 1.0f, "confidence is in 0..1");
        if (edge < nearestEdge) {
            nearestEdge = edge;
            nearestConfidence = hit.confidence;
        }
    }
    check(
        checked > 10 && hits == checked, "every floor pixel down the middle reflects the wall (" +
                                             std::to_string(hits) + " of " + std::to_string(checked) + ")");
    // Near the top of the screen, confidence fades to nothing.
    {
        const ReflectionHit top =
            traceReflection(s.depth, s.w, s.h, s.pixelOf({0, 0, -4.9f}), s.projection, s.inverseProjection, up, steps);
        const float edge = top.hit ? std::min(top.uv.y, 1 - top.uv.y) : 0.0f;
        check(
            !top.hit || top.confidence <= 0.05f || edge > 0.1f,
            "a reflection at the screen's edge has (almost) no confidence");
    }
    check(nearestConfidence < 1.0f || nearestEdge >= 0.1f, "confidence fades in the outer 10% of the screen");
    // Without the wall, the rays leave the screen: no hit.
    Scene open(eye, {0, 0.6f, -5});
    open.boxes = {kFloor};
    open.trace();
    int stray = 0;
    for (uint32_t y = open.h / 2 + 10; y < open.h; y += 7)
        stray += traceReflection(
                     open.depth, open.w, open.h, {open.w / 2, y}, open.projection, open.inverseProjection, up, steps)
                     .hit;
    check(stray == 0, "rays that leave the screen don't hit");
    // A thin post near the camera covering the middle of the screen's upper half: the rays
    // pass far behind it (more than the thickness), so they don't hit it.
    Scene post = open;
    for (uint32_t y = 0; y < post.h * 45 / 100; ++y)
        for (uint32_t x = post.w / 2 - 6; x <= post.w / 2 + 6; ++x) {
            const glm::vec4 clip = post.projection * glm::vec4(0, 0, -2.0f, 1.0f);
            post.depth[size_t(y) * post.w + x] = clip.z / clip.w;
        }
    stray = 0;
    for (uint32_t y = post.h / 2 + 10; y < post.h; y += 7)
        stray += traceReflection(
                     post.depth, post.w, post.h, {post.w / 2, y}, post.projection, post.inverseProjection, up, steps)
                     .hit;
    check(stray == 0, "rays passing more than the thickness behind a thin post don't hit it");
    // Sky and rays towards the camera.
    check(
        !traceReflection(s.depth, s.w, s.h, {s.w / 2, 0}, s.projection, s.inverseProjection, up, steps).hit ||
            s.depth[s.w / 2] > 0.0f,
        "sky doesn't reflect");
    check(
        !traceReflection(
             s.depth, s.w, s.h, s.pixelOf({0, 1, -5}), s.projection, s.inverseProjection, s.toView({0, 0, 1}), steps)
             .hit,
        "a wall facing the camera reflects what's behind the camera: no hit");
    check(
        std::abs(reflectionFirstStep(16, 30.0f) * (std::pow(1.05f, 16.0f) - 1.0f) / 0.05f - 30.0f) < 1e-3f,
        "the steps add up to the distance");
    check(
        reflectionRoughnessWeight(0.2f) == 1.0f && reflectionRoughnessWeight(0.7f) == 0.0f &&
            reflectionRoughnessWeight(0.475f) > 0.4f && reflectionRoughnessWeight(0.475f) < 0.6f,
        "roughness fades the reflection between 0.35 and 0.6");
    check(
        screenReflectionSteps(ScreenReflections::Off) == 0 && screenReflectionSteps(ScreenReflections::Low) == 16 &&
            screenReflectionSteps(ScreenReflections::Medium) == 32,
        "steps per quality");
}

void testMotionBlur() {
    const glm::vec2 size(1920, 1080);
    const float frame = 1.0f / 60.0f;
    check(blurVector(glm::vec2(0.0f), 1.0f, frame, size) == glm::vec2(0.0f), "no motion, no blur");
    const glm::vec2 v = blurVector({10.0f / 1920.0f, 0.0f}, 1.0f, frame, size);
    check(std::abs(v.x - 5.0f) < 1e-3f && v.y == 0.0f, "10 px a frame at 60 fps is a 5 px blur (1/120 s shutter)");
    const glm::vec2 half = blurVector({10.0f / 1920.0f, 0.0f}, 1.0f, 2.0f * frame, size);
    check(std::abs(half.x - 2.5f) < 1e-3f, "twice the frame time halves the blur");
    const glm::vec2 fast = blurVector({0.5f, 0.5f}, 1.0f, frame, size);
    check(std::abs(glm::length(fast) - kMotionBlurMaxPixels) < 1e-3f, "blur is at most 32 px");
    check(blurVector({10.0f / 1920.0f, 0.0f}, 0.0f, frame, size) == glm::vec2(0.0f), "strength 0 blurs nothing");
    check(blurVector({0.5f / 1920.0f, 0.0f}, 1.0f, frame, size) == glm::vec2(0.0f), "under half a pixel blurs nothing");
    check(blurVector({0.1f, 0.0f}, 1.0f, 0.0f, size) == glm::vec2(0.0f), "no frame time, no blur");
    const glm::vec2 strength = blurVector({10.0f / 1920.0f, 0.0f}, 0.5f, frame, size);
    check(std::abs(strength.x - 2.5f) < 1e-3f, "strength scales the blur");
}

void testDepthOfField() {
    const float focus = 10.0f, range = 2.0f, max = 12.0f, falloff = dofFalloff(focus);
    check(falloff == 10.0f && dofFalloff(0.3f) == 1.0f, "the falloff is the focus distance, at least 1 m");
    check(circleOfConfusion(10.0f, focus, range, max, falloff) == 0.0f, "sharp at the focus");
    check(
        circleOfConfusion(12.0f, focus, range, max, falloff) == 0.0f &&
            circleOfConfusion(8.0f, focus, range, max, falloff) == 0.0f,
        "sharp at the range's edges");
    check(
        std::abs(circleOfConfusion(17.0f, focus, range, max, falloff) - 6.0f) < 1e-4f,
        "halfway down the falloff: half");
    check(circleOfConfusion(22.0f, focus, range, max, falloff) == max, "the falloff's end: the most blur");
    check(circleOfConfusion(500.0f, focus, range, max, falloff) == max, "past it: still the most");
    check(
        circleOfConfusion(std::numeric_limits<float>::infinity(), focus, range, max, falloff) == max,
        "the sky blurs the most, behind everything");
    check(
        circleOfConfusion(3.0f, focus, range, max, falloff) < 0.0f &&
            std::abs(circleOfConfusion(3.0f, focus, range, max, falloff) + 6.0f) < 1e-4f,
        "in front of the focus: negative");
    check(circleOfConfusion(30.0f, focus, range, 0.0f, falloff) == 0.0f, "no blur asked: none");
    check(
        dofRadiusPixels(12.0f, 1080) == 12.0f && dofRadiusPixels(12.0f, 540) == 6.0f &&
            dofRadiusPixels(12.0f, 2160) == 24.0f,
        "dof_blur is pixels at 1080 lines, scaled with the resolution");
}
}   // namespace

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "";
    if (which == "contact_shadows")
        testContactShadows();
    else if (which == "screen_reflections")
        testScreenReflections();
    else if (which == "motion_blur")
        testMotionBlur();
    else if (which == "depth_of_field")
        testDepthOfField();
    else {
        std::fprintf(
            stderr, "usage: screen_effects_test contact_shadows|screen_reflections|motion_blur|depth_of_field\n");
        return 2;
    }
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
