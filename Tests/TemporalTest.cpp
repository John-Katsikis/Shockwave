// Motion vectors and temporal anti-aliasing (G09, G10; docs/TemporalAntiAliasing.md): the
// jitter sequence, how a jittered projection moves the image, motion for a moving camera,
// reprojecting into the previous jittered frame, when history resets, and the resolve's
// maths (YCoCg, clipping to the neighbourhood box, history weights, depth tolerance). No
// GPU: these run anywhere. The expected values are worked out by hand here.

#include <Engine/Camera.h>
#include <Renderer/ScreenSpace.h>
#include <Renderer/Temporal.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <string>

using namespace shockwave;

namespace {
int failures = 0;
void check(bool value, const std::string& message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}
bool near(float a, float b, float eps = 1e-5f) { return std::abs(a - b) <= eps; }
bool near(const glm::vec2& a, const glm::vec2& b, float eps = 1e-5f) {
    return glm::all(glm::lessThan(glm::abs(a - b), glm::vec2(eps)));
}
bool near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-5f) {
    return glm::all(glm::lessThan(glm::abs(a - b), glm::vec3(eps)));
}

glm::vec2 project(const glm::mat4& viewProjection, const glm::vec3& world) {
    return screenUvFromClip(viewProjection * glm::vec4(world, 1.0f));
}

void testHalton() {
    check(near(halton(1, 2), 0.5f) && near(halton(2, 2), 0.25f) && near(halton(3, 2), 0.75f) &&
              near(halton(4, 2), 0.125f),
          "halton base 2");
    check(near(halton(1, 3), 1.0f / 3.0f) && near(halton(2, 3), 2.0f / 3.0f) && near(halton(3, 3), 1.0f / 9.0f) &&
              near(halton(4, 3), 4.0f / 9.0f),
          "halton base 3");
    check(halton(0, 2) == 0.0f, "halton of 0");

    // Frame 0 is index 1: (0.5 - 0.5, 1/3 - 0.5).
    check(near(taaJitter(0), {0.0f, -1.0f / 6.0f}), "frame 0 jitter");
    check(near(taaJitter(1), {-0.25f, 1.0f / 6.0f}), "frame 1 jitter");
    // Index 8: 1/16 in base 2, 22 (8/9) in base 3.
    check(near(taaJitter(7), {0.0625f - 0.5f, 8.0f / 9.0f - 0.5f}), "frame 7 jitter");
    check(taaJitter(kTaaJitterCycle) == taaJitter(0) && taaJitter(kTaaJitterCycle + 3) == taaJitter(3),
          "jitter repeats every cycle");
    glm::vec2 sum(0.0f);
    for (uint32_t i = 0; i < kTaaJitterCycle; ++i) {
        const glm::vec2 j = taaJitter(i);
        check(j.x >= -0.5f && j.x <= 0.5f && j.y >= -0.5f && j.y <= 0.5f, "jitter within half a pixel");
        sum += j;
        for (uint32_t k = 0; k < i; ++k) check(taaJitter(k) != j, "jitter samples are distinct");
    }
    check(glm::length(sum / float(kTaaJitterCycle)) < 0.1f, "jitter is roughly centred over a cycle");
}

void testJitterProjection() {
    const glm::vec2 extent(320.0f, 180.0f);
    const glm::mat4 view = glm::lookAt(glm::vec3(1.0f, 2.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 perspective = perspectiveReverseZ(glm::radians(60.0f), extent.x / extent.y, 0.1f);
    const glm::mat4 ortho = orthographicReverseZ(4.0f, 2.25f, 0.1f, 100.0f);
    const glm::vec2 jitter(0.25f, -0.375f);
    for (const glm::mat4& projection : {perspective, ortho}) {
        const glm::mat4 jittered = jitterProjection(projection, jitter, extent);
        for (const glm::vec3& p : {glm::vec3(0.0f), glm::vec3(0.7f, -0.3f, 1.2f), glm::vec3(-1.5f, 0.8f, -2.0f)}) {
            const glm::vec2 a = project(projection * view, p), b = project(jittered * view, p);
            // Positive J moves the image right and down by J pixels.
            check(near((b - a) * extent, jitter, 1e-3f), "jittered projection moves the image J pixels");
        }
        // Depth is untouched: same z and w.
        const glm::vec4 c0 = projection * view * glm::vec4(0.7f, -0.3f, 1.2f, 1.0f);
        const glm::vec4 c1 = jittered * view * glm::vec4(0.7f, -0.3f, 1.2f, 1.0f);
        check(near(c0.z, c1.z) && near(c0.w, c1.w), "jitter leaves depth alone");
    }
    check(jitterProjection(perspective, glm::vec2(0.0f), extent) == perspective, "zero jitter is the identity");
}

void testMotion() {
    // A camera moving 0.5 m right between frames: a static point moves left on screen.
    const glm::vec2 extent(400.0f, 200.0f);
    const glm::mat4 projection = perspectiveReverseZ(glm::radians(90.0f), extent.x / extent.y, 0.1f);
    const glm::mat4 previousView = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0, 1, 0));
    const glm::mat4 currentView =
        glm::lookAt(glm::vec3(0.5f, 0.0f, 0.0f), glm::vec3(0.5f, 0.0f, -1.0f), glm::vec3(0, 1, 0));
    const glm::vec3 point(0.0f, 0.0f, -4.0f);
    const glm::vec2 previous = project(projection * previousView, point);
    const glm::vec2 current = project(projection * currentView, point);
    // fov 90, aspect 2: x_ndc = (x / aspect) / depth. Previous centre; now x = -0.5 at 4 m:
    // ndc -0.0625, u = 0.46875.
    check(near(previous, {0.5f, 0.5f}) && near(current, {0.46875f, 0.5f}), "projected positions");
    const glm::vec2 motion = motionVector(current, previous);
    check(near(motion, {-0.03125f, 0.0f}), "motion is current minus previous, in UV");
    check(near(motion.x * extent.x, -12.5f, 1e-3f), "12.5 pixels left");

    // Reprojecting into last frame's jittered image: the current sample at its jittered uv
    // lands on the previous frame's jittered position of the same point.
    const glm::vec2 currentJitter = taaJitter(3), previousJitter = taaJitter(2);
    const glm::vec2 currentJittered =
        project(jitterProjection(projection, currentJitter, extent) * currentView, point);
    const glm::vec2 previousJittered =
        project(jitterProjection(projection, previousJitter, extent) * previousView, point);
    // The motion target is written at the jittered uv; motion itself is unjittered.
    const glm::vec2 history = previousHistoryUv(currentJittered, motion, previousJitter, extent, currentJitter, extent);
    check(near(history, previousJittered, 1e-5f), "previous history uv lands on last frame's jittered sample");
    // Static camera, no motion: only the jitter difference remains.
    check(near(previousHistoryUv({0.3f, 0.6f}, glm::vec2(0.0f), {0.5f, 0.0f}, extent, {0.0f, 0.0f}, extent),
               {0.3f + 0.5f / 400.0f, 0.6f}),
          "jitter difference alone");
}

void testResets() {
    const RenderViewId a = RenderViewId::create(), b = RenderViewId::create();
    TemporalViewState none;
    TemporalViewState current{true, a, {640, 360}, false};
    check(historyReset(none, current, false) == HistoryReset::FirstFrame, "nothing before: first frame");
    check(historyReset(current, current, false) == HistoryReset::None, "same view carries on");
    TemporalViewState other = current;
    other.view = b;
    check(historyReset(current, other, false) == HistoryReset::ViewChanged, "another view");
    TemporalViewState resized = current;
    resized.extent = {640, 361};
    check(historyReset(current, resized, false) == HistoryReset::Resized, "resized");
    TemporalViewState ortho = current;
    ortho.orthographic = true;
    check(historyReset(current, ortho, false) == HistoryReset::ProjectionChanged, "projection changed");
    check(historyReset(current, current, true) == HistoryReset::CameraCut, "camera cut");
    // The first matching reason wins.
    other.extent = {1, 1};
    check(historyReset(current, other, true) == HistoryReset::ViewChanged, "view change reported before resize");
    check(std::string(historyResetName(HistoryReset::None)) == "none" &&
              std::string(historyResetName(HistoryReset::CameraCut)) == "camera cut" &&
              std::string(historyResetName(HistoryReset::ModeChanged)) == "mode changed",
          "reset names");
}

void testResolve() {
    check(near(taaHistoryWeight(0.0f, 0.0f), 0.9f), "rest weight");
    check(near(taaHistoryWeight(2.0f, 0.0f), 0.8f), "half way to the motion weight");
    check(near(taaHistoryWeight(4.0f, 0.0f), 0.7f) && near(taaHistoryWeight(40.0f, 0.0f), 0.7f),
          "motion weight from 4 pixels on");
    check(near(taaHistoryWeight(0.0f, 1.0f), 0.0f) && near(taaHistoryWeight(0.0f, 0.5f), 0.45f),
          "reactive pulls the weight to 0");
    check(near(taaHistoryWeight(0.0f, 2.0f), 0.0f) && near(taaHistoryWeight(0.0f, -1.0f), 0.9f), "reactive clamped");

    check(near(taaDepthTolerance(0.2f), 0.01f) && near(taaDepthTolerance(50.0f), 0.5f), "depth tolerance");

    // White is (1, 0, 0) in YCoCg; red is (1/4, 1/2, -1/4).
    check(near(rgbToYCoCg({1.0f, 1.0f, 1.0f}), {1.0f, 0.0f, 0.0f}), "white in YCoCg");
    check(near(rgbToYCoCg({1.0f, 0.0f, 0.0f}), {0.25f, 0.5f, -0.25f}), "red in YCoCg");
    for (const glm::vec3& c : {glm::vec3(0.2f, 0.7f, 0.1f), glm::vec3(3.0f, 0.5f, 9.0f), glm::vec3(0.0f)})
        check(near(yCoCgToRgb(rgbToYCoCg(c)), c, 1e-5f), "YCoCg round trip");

    const glm::vec3 lo(0.0f), hi(1.0f);
    check(clipToBox({0.3f, 0.6f, 0.9f}, lo, hi) == glm::vec3(0.3f, 0.6f, 0.9f), "inside is unchanged");
    // From the centre (0.5) out to (2.5, 0.5, 0.5): offset 2 along x, half-extent 0.5, so a
    // quarter of the way: (1, 0.5, 0.5).
    check(near(clipToBox({2.5f, 0.5f, 0.5f}, lo, hi), {1.0f, 0.5f, 0.5f}), "clipped along x");
    // Offset (1.5, 0.5, 0): x is furthest out (3 extents), so a third of the offset.
    check(near(clipToBox({2.0f, 1.0f, 0.5f}, lo, hi), {1.0f, 2.0f / 3.0f, 0.5f}), "clipped along the line");
    // A flat box (lo == hi on an axis) doesn't divide by zero.
    const glm::vec3 flat = clipToBox({0.5f, 0.9f, 0.5f}, {0.0f, 0.5f, 0.0f}, {1.0f, 0.5f, 1.0f});
    check(std::isfinite(flat.y) && near(flat.y, 0.5f, 1e-4f), "flat box");
}

}   // namespace

int main() {
    testHalton();
    testJitterProjection();
    testMotion();
    testResets();
    testResolve();
    if (failures) return 1;
    std::printf("temporal: all checks passed\n");
    return 0;
}
