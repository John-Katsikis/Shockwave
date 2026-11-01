// Occlusion culling contract (G12; docs/GraphicsImplementation.md 16.2): the hierarchical
// depth keeps the farthest depth, odd sizes lose no texel, a sky gap keeps an object
// visible, projections that can't be judged are drawn, and a brute-force reference agrees
// that nothing culled could have been seen. No GPU.
#include <Renderer/Culling.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cstdio>
#include <random>
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

void testReduction() {
    // 5 x 3: odd both ways. Level 0 is 3 x 2; the last column and row reduce one child.
    const std::vector<float> depth = {0.9f, 0.8f, 0.7f, 0.6f, 0.5f,   //
                                      0.4f, 0.9f, 0.9f, 0.9f, 0.3f,   //
                                      0.9f, 0.9f, 0.9f, 0.9f, 0.2f};
    const auto hzb = buildHzb(depth, 5, 3);
    check(hzb.size() == 3, "5 x 3 reduces to 3 x 2, 2 x 1, 1 x 1");
    check(hzb[0].width == 3 && hzb[0].height == 2, "level 0 rounds up");
    check(hzb[0].depth[0] == 0.4f && hzb[0].depth[1] == 0.6f && hzb[0].depth[2] == 0.3f, "top row keeps the farthest");
    check(hzb[0].depth[3] == 0.9f && hzb[0].depth[5] == 0.2f, "the odd last row and column count");
    check(hzb.back().depth[0] == 0.2f, "the top level is the farthest of all");
    // A single sky texel (0) under a wall keeps everything behind the wall visible.
    std::vector<float> wall(64 * 64, 0.5f);
    wall[40 * 64 + 33] = 0.0f;
    const auto gap = buildHzb(wall, 64, 64);
    for (const HzbLevel& level : gap) {
        float least = 1.0f;
        for (float d : level.depth) least = std::min(least, d);
        check(least == 0.0f, "a sky gap reaches every level");
    }
}

void testProjection() {
    const glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 5), glm::vec3(0), glm::vec3(0, 1, 0));
    const glm::mat4 vp = reverseZ(glm::radians(60.0f), 16.0f / 9.0f, 0.1f) * view;
    const ScreenRect centre = projectBounds({0, 0, 0, 0.5f}, vp, 640, 360);
    check(centre.valid, "an object in front is judged");
    check(centre.min.x < 320 && centre.max.x > 320 && centre.min.y < 180 && centre.max.y > 180, "it's centred");
    // Its nearest corner is 4.5 m away: depth 0.1 / 4.5.
    check(std::abs(centre.nearestDepth - 0.1f / 4.5f) < 1e-5f, "nearest corner depth");
    const ScreenRect up = projectBounds({0, 1, 0, 0.1f}, vp, 640, 360);
    check(up.valid && up.max.y < 180, "up is the top of the image");
    check(!projectBounds({0, 0, 4.95f, 0.2f}, vp, 640, 360).valid, "crossing the near plane is drawn");
    check(!projectBounds({0, 0, 6, 0.5f}, vp, 640, 360).valid, "behind the camera is drawn");
    check(!projectBounds({0, 0, 5, 1.0f}, vp, 640, 360).valid, "the camera inside it is drawn");
}

void testOcclusion() {
    // A wall at depth 0.5 everywhere: things behind it are hidden, things in front aren't.
    std::vector<float> depth(640 * 360, 0.5f);
    auto hzb = buildHzb(depth, 640, 360);
    ScreenRect rect{true, {100, 100}, {140, 130}, 0.4f};
    check(hzbOccluded(hzb, rect), "behind the wall is occluded");
    rect.nearestDepth = 0.5f;
    check(!hzbOccluded(hzb, rect), "touching the wall isn't");
    rect.nearestDepth = 0.6f;
    check(!hzbOccluded(hzb, rect), "in front of it isn't");
    rect.valid = false;
    rect.nearestDepth = 0.1f;
    check(!hzbOccluded(hzb, rect), "an invalid rectangle is never occluded");
    // A hole the size of one pixel anywhere under the rectangle keeps it visible.
    for (const glm::ivec2 hole : {glm::ivec2(100, 100), glm::ivec2(140, 130), glm::ivec2(121, 117)}) {
        std::vector<float> holed = depth;
        holed[size_t(hole.y) * 640 + hole.x] = 0.0f;
        const auto h = buildHzb(holed, 640, 360);
        check(!hzbOccluded(h, {true, {100, 100}, {140, 130}, 0.4f}), "a one-pixel gap keeps it visible");
    }
    // Large rectangles and ones past the edge.
    check(hzbOccluded(hzb, {true, {-50, -50}, {700, 400}, 0.3f}), "a rectangle past the screen is clamped");
    check(!hzbOccluded(hzb, {true, {700, 10}, {720, 20}, 0.3f}), "wholly off screen is left to frustum culling");
    check(hzbLevelFor({true, {0, 0}, {8, 8}, 0}, 10) == 0, "small rectangles read level 0");
    check(hzbLevelFor({true, {0, 0}, {640, 360}, 0}, 10) >= 6, "large ones read a coarse level");
}

// Random walls with gaps and random objects: whenever the test says occluded, every depth
// pixel under the object's rectangle must be nearer than it (so not one of its pixels could
// pass the depth test).
void testAgainstBruteForce() {
    std::mt19937 random(7);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const uint32_t w = 97, h = 61;   // odd on purpose
    int culled = 0;
    for (int trial = 0; trial < 300; ++trial) {
        std::vector<float> depth(size_t(w) * h);
        const float wall = 0.2f + 0.6f * unit(random);
        for (auto& d : depth) d = unit(random) < 0.02f ? 0.0f : wall + 0.1f * unit(random);
        const auto hzb = buildHzb(depth, w, h);
        for (int object = 0; object < 20; ++object) {
            ScreenRect rect;
            rect.valid = true;
            rect.min = {unit(random) * w, unit(random) * h};
            rect.max = rect.min + glm::vec2(unit(random) * 30.0f, unit(random) * 30.0f);
            rect.nearestDepth = unit(random);
            if (!hzbOccluded(hzb, rect)) continue;
            ++culled;
            float least = 1.0f;
            for (uint32_t y = uint32_t(std::max(rect.min.y, 0.0f)); y <= std::min(uint32_t(rect.max.y), h - 1); ++y)
                for (uint32_t x = uint32_t(std::max(rect.min.x, 0.0f)); x <= std::min(uint32_t(rect.max.x), w - 1); ++x)
                    least = std::min(least, depth[size_t(y) * w + x]);
            check(rect.nearestDepth < least, "culled only when every pixel under it is nearer");
        }
    }
    check(culled > 50, "the random scenes cull something: " + std::to_string(culled));
}
}   // namespace

int main() {
    testReduction();
    testProjection();
    testOcclusion();
    testAgainstBruteForce();
    if (failures) return 1;
    std::printf("culling: all checks passed\n");
    return 0;
}
