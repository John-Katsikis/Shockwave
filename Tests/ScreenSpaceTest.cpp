// The screen-space coordinate contract (G03, docs/GraphicsImplementation.md 7.2): screen UV
// from clip space, positions back from reverse-Z depth through the inverse projection, and
// the octahedral normals the surface prepass stores. Shaders/screen.glsl is the same code in
// GLSL; the GPU side is checked through the ambient occlusion fixtures.

#include <Engine/Camera.h>
#include <Renderer/ScreenSpace.h>

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
bool near(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }
bool near(const glm::vec3& a, const glm::vec3& b, float tolerance) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
}

// Projects a view-space point, then brings it back from its screen UV and depth, as the
// prepass's readers do. Relative tolerance: depth precision falls with distance.
void roundTrip(const glm::mat4& projection, const glm::vec3& point, const std::string& what) {
    const glm::vec4 clip = projection * glm::vec4(point, 1.0f);
    const glm::vec2 uv = screenUvFromClip(clip);
    const float depth = clip.z / clip.w;
    glm::vec3 back(0.0f);
    const bool ok = viewPositionFromDepth(uv, depth, glm::inverse(projection), back);
    check(ok, what + ": reconstructs");
    const float tolerance = 1e-4f * std::max(1.0f, glm::length(point));
    check(ok && near(back, point, tolerance), what + ": position round trip");
}

void uvChecks() {
    const glm::mat4 projection = perspectiveReverseZ(glm::radians(70.0f), 16.0f / 9.0f, 0.05f);
    // The view's corners: u right, v down, (0, 0) top-left (the image's first row).
    auto uvOf = [&](const glm::vec3& p) { return screenUvFromClip(projection * glm::vec4(p, 1.0f)); };
    const float t = std::tan(glm::radians(35.0f)), a = 16.0f / 9.0f;
    const glm::vec2 topLeft = uvOf({-a * t * 5.0f, t * 5.0f, -5.0f});
    const glm::vec2 bottomRight = uvOf({a * t * 5.0f, -t * 5.0f, -5.0f});
    const glm::vec2 center = uvOf({0.0f, 0.0f, -5.0f});
    check(near(topLeft.x, 0.0f, 1e-5f) && near(topLeft.y, 0.0f, 1e-5f), "top-left corner is uv (0, 0)");
    check(near(bottomRight.x, 1.0f, 1e-5f) && near(bottomRight.y, 1.0f, 1e-5f), "bottom-right corner is uv (1, 1)");
    check(near(center.x, 0.5f, 1e-6f) && near(center.y, 0.5f, 1e-6f), "the view direction is the centre");
    check(uvOf({1.0f, 0.0f, -5.0f}).x > 0.5f, "u grows to the right");
    check(uvOf({0.0f, 1.0f, -5.0f}).y < 0.5f, "v grows downwards");
}

void reconstructionChecks() {
    const float nearPlane = 0.05f;
    const glm::mat4 wide = perspectiveReverseZ(glm::radians(70.0f), 16.0f / 9.0f, nearPlane);
    // Reverse-Z: the near plane is depth 1, infinitely far is 0.
    const glm::vec4 atNear = wide * glm::vec4(0.0f, 0.0f, -nearPlane, 1.0f);
    check(near(atNear.z / atNear.w, 1.0f, 1e-6f), "the near plane is depth 1");
    for (const glm::vec3& p : {glm::vec3(0.0f, 0.0f, -nearPlane), glm::vec3(0.3f, -0.2f, -1.0f),
                                glm::vec3(-12.0f, 4.0f, -40.0f), glm::vec3(150.0f, -60.0f, -900.0f)})
        roundTrip(wide, p, "perspective (" + std::to_string(p.z) + ")");
    // The corners of the view, near and far.
    const float t = std::tan(glm::radians(35.0f)), a = 16.0f / 9.0f;
    for (float d : {0.06f, 3.0f, 500.0f})
        for (float sx : {-1.0f, 1.0f})
            for (float sy : {-1.0f, 1.0f})
                roundTrip(wide, {sx * a * t * d * 0.999f, sy * t * d * 0.999f, -d}, "corner at " + std::to_string(d));
    // Far away the infinite projection's depth is near / distance.
    const glm::vec4 far = wide * glm::vec4(0.0f, 0.0f, -2000.0f, 1.0f);
    check(near(infinitePerspectiveDistance(far.z / far.w, nearPlane), 2000.0f, 0.5f), "distance from far depth");
    // A tall, narrow view and a very wide one: aspect is the projection's business only.
    roundTrip(perspectiveReverseZ(glm::radians(90.0f), 0.4f, 0.1f), {0.5f, 2.0f, -3.0f}, "non-square (tall)");
    roundTrip(perspectiveReverseZ(glm::radians(40.0f), 3.2f, 0.1f), {-6.0f, 0.5f, -7.0f}, "non-square (wide)");
    // Orthographic, as editor side views draw.
    const glm::mat4 ortho = orthographicReverseZ(8.0f, 4.5f, 0.1f, 200.0f);
    const glm::vec4 orthoNear = ortho * glm::vec4(0.0f, 0.0f, -0.1f, 1.0f);
    const glm::vec4 orthoFar = ortho * glm::vec4(0.0f, 0.0f, -200.0f, 1.0f);
    check(near(orthoNear.z, 1.0f, 1e-6f) && near(orthoFar.z, 0.0f, 1e-6f), "orthographic: near 1, far 0");
    for (const glm::vec3& p : {glm::vec3(-7.9f, 4.4f, -0.2f), glm::vec3(3.0f, -1.0f, -50.0f), glm::vec3(7.9f, -4.4f, -199.0f)})
        roundTrip(ortho, p, "orthographic (" + std::to_string(p.z) + ")");
    // Background: depth 0 (the clear) has no position, and nothing is divided by zero.
    glm::vec3 none(0.0f);
    check(!viewPositionFromDepth({0.5f, 0.5f}, 0.0f, glm::inverse(wide), none), "depth 0 is background");
    check(!viewPositionFromDepth({0.5f, 0.5f}, -0.1f, glm::inverse(wide), none), "negative depth is background");
    check(!viewPositionFromDepth({0.5f, 0.5f}, std::nanf(""), glm::inverse(wide), none), "NaN depth is background");
}

// Render scale changes the image's size, never the contract: the same point lands on the
// same UV, so a texel's centre there reconstructs (to within the texel) the same point.
void renderScaleChecks() {
    const glm::mat4 projection = perspectiveReverseZ(glm::radians(70.0f), 16.0f / 9.0f, 0.05f);
    const glm::vec3 point(1.2f, -0.4f, -6.0f);
    const glm::vec4 clip = projection * glm::vec4(point, 1.0f);
    const glm::vec2 uv = screenUvFromClip(clip);
    for (float scale : {0.5f, 1.0f, 2.0f}) {
        const glm::vec2 size = glm::vec2(1280.0f, 720.0f) * scale;
        const glm::vec2 texel = glm::floor(uv * size);
        const glm::vec2 centre = (texel + 0.5f) / size;
        glm::vec3 back(0.0f);
        viewPositionFromDepth(centre, clip.z / clip.w, glm::inverse(projection), back);
        // A texel spans 2 tan(35) * 6 / 720 / scale metres at this distance.
        const float spread = 2.0f * std::tan(glm::radians(35.0f)) * 6.0f / size.y;
        check(near(back, point, spread), "render scale " + std::to_string(scale) + ": within a texel");
    }
}

void octahedralChecks() {
    float worst = 0.0f;
    // A spread of directions, including the axes and the folds of the octahedron.
    for (int i = 0; i < 2000; ++i) {
        const float z = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / 2000.0f;
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const float phi = static_cast<float>(i) * 2.39996323f;
        const glm::vec3 n(r * std::cos(phi), r * std::sin(phi), z);
        const glm::vec2 e = octEncode(n);
        check(std::abs(e.x) <= 1.0f && std::abs(e.y) <= 1.0f, "octahedral codes are in -1..1");
        worst = std::max(worst, glm::length(octDecode(e) - n));
        // The prepass stores 16-bit signed normalized values: still well under a degree.
        const glm::vec2 q = glm::round(e * 32767.0f) / 32767.0f;
        check(glm::dot(octDecode(q), n) > std::cos(glm::radians(0.05f)), "16-bit octahedral normals");
    }
    check(worst < 1e-5f, "octahedral round trip");
    for (const glm::vec3& axis : {glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, -1, 0),
                                  glm::vec3(0, 0, 1), glm::vec3(0, 0, -1)})
        check(near(octDecode(octEncode(axis)), axis, 1e-6f), "octahedral axes");
}
}   // namespace

int main() {
    uvChecks();
    reconstructionChecks();
    renderScaleChecks();
    octahedralChecks();
    if (failures) {
        std::fprintf(stderr, "%d screen-space check(s) failed\n", failures);
        return 1;
    }
    std::puts("screen-space contract: all checks passed");
    return 0;
}
