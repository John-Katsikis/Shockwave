// Diffuse probe volumes (G06; docs/GraphicsImplementation.md 10.3): interpolation inside a
// volume, invalid and hidden probes dropping out, the nearest-visible fallback, priorities,
// boundary blending, rotated volumes, the environment outside, and smooth changes along a
// path through a doorway.

#include <Assets/LightingData.h>

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
std::string text(float v) {
    char out[32];
    std::snprintf(out, sizeof(out), "%.4f", v);
    return out;
}
const glm::vec3 kUp(0, 1, 0);
float lit(const ProbeSample& s) { return shEvaluate(s.sh, kUp).x; }

// A volume of counts probes over the box, each lit by value(x, y, z), seeing open space
// out to maxDistance in every direction.
LightingProbeVolume grid(glm::uvec3 counts, glm::vec3 halfExtents, float (*value)(uint32_t, uint32_t, uint32_t)) {
    LightingProbeVolume v;
    v.binding = newBindingId();
    v.halfExtents = halfExtents;
    v.counts = counts;
    v.blendDistance = 0.0f;
    v.maxDistance = 100.0f;   // open space: farther than any cell diagonal here
    v.probes.resize(size_t(counts.x) * counts.y * counts.z);
    for (uint32_t z = 0; z < counts.z; ++z)
        for (uint32_t y = 0; y < counts.y; ++y)
            for (uint32_t x = 0; x < counts.x; ++x) {
                LightingProbe& p = v.probes[x + counts.x * (y + size_t(counts.y) * z)];
                p.sh = shConstant(glm::vec3(value(x, y, z)));
                for (glm::vec2& d : p.distance) d = {v.maxDistance, v.maxDistance * v.maxDistance};
            }
    return v;
}
LightingProbe& probe(LightingProbeVolume& v, uint32_t x, uint32_t y, uint32_t z) {
    return v.probes[x + v.counts.x * (y + size_t(v.counts.y) * z)];
}
}   // namespace

int main() {
    LightingProbeSet set;
    set.environment = shConstant(glm::vec3(0.05f));

    // Outside every volume: the environment, with no coverage.
    {
        const ProbeSample s = sampleProbes(set, {0, 0, 0});
        check(s.coverage == 0.0f && std::abs(lit(s) - 0.05f) < 1e-5f, "outside: environment");
    }
    // One cell, constant probes: the constant everywhere inside, at every normal.
    {
        set.volumes = {grid({2, 2, 2}, {1, 1, 1}, [](uint32_t, uint32_t, uint32_t) { return 0.6f; })};
        for (const glm::vec3& p : {glm::vec3(0), glm::vec3(0.9f, -0.9f, 0.3f), glm::vec3(-1, 1, 1)}) {
            const ProbeSample s = sampleProbes(set, p);
            check(s.coverage == 1.0f, "one cell: covered");
            for (const glm::vec3& n : {kUp, glm::vec3(1, 0, 0), glm::normalize(glm::vec3(-1, -1, 0.5f))})
                check(std::abs(shEvaluate(s.sh, n).x - 0.6f) < 1e-4f, "one cell: constant stays constant");
        }
    }
    // Trilinear between different probes: halfway is the average, and the change is smooth.
    {
        set.volumes = {grid({2, 1, 1}, {1, 1, 1}, [](uint32_t x, uint32_t, uint32_t) { return x ? 1.0f : 0.0f; })};
        check(std::abs(lit(sampleProbes(set, {0, 0, 0})) - 0.5f) < 1e-3f, "trilinear: halfway is the average");
        float previous = -1.0f, worstStep = 0.0f;
        for (int i = 0; i <= 100; ++i) {
            const float v = lit(sampleProbes(set, {-1.0f + 0.02f * float(i), 0, 0}));
            if (previous >= 0.0f) worstStep = std::max(worstStep, std::abs(v - previous));
            previous = v;
        }
        check(worstStep < 0.03f, "trilinear: smooth, largest step " + text(worstStep));
    }
    // Invalid probes never contribute; with all eight invalid, the nearest valid probe
    // that sees the point stands in.
    {
        auto v = grid({4, 1, 1}, {1.5f, 1, 1}, [](uint32_t x, uint32_t, uint32_t) { return 0.1f + 0.2f * float(x); });
        probe(v, 1, 0, 0).valid = false;
        set.volumes = {v};
        // Between probes 1 (invalid) and 2: only probe 2 counts.
        check(std::abs(lit(sampleProbes(set, {-0.2f, 0, 0})) - 0.5f) < 1e-3f, "invalid: dropped from the blend");
        probe(v, 2, 0, 0).valid = false;
        set.volumes = {v};
        const ProbeSample s = sampleProbes(set, {0.0f, 0, 0});
        check(s.fallback && s.coverage == 1.0f, "invalid: fallback to the nearest valid probe");
        const float value = lit(s);
        check(
            std::abs(value - 0.1f) < 1e-3f || std::abs(value - 0.7f) < 1e-3f, "invalid: the fallback is a valid probe");
        for (LightingProbe& p : v.probes) p.valid = false;
        set.volumes = {v};
        check(sampleProbes(set, {0, 0, 0}).coverage == 0.0f, "invalid: none valid, environment");
    }
    // A wall: probe 0 (bright) sees a surface 0.3 m away towards +x, so a point past it is
    // hidden and probe 1 (dark) decides, rather than light leaking through.
    {
        auto v = grid({2, 1, 1}, {1, 1, 1}, [](uint32_t x, uint32_t, uint32_t) { return x ? 0.0f : 1.0f; });
        for (uint32_t t = 0; t < kProbeVisibilityTexels; ++t) {
            const glm::vec3 d = octahedralDecode(
                glm::vec2(float(t % kProbeVisibilitySize) + 0.5f, float(t / kProbeVisibilitySize) + 0.5f) /
                float(kProbeVisibilitySize));
            if (d.x > 0.3f) probe(v, 0, 0, 0).distance[t] = {0.3f, 0.09f};
        }
        set.volumes = {v};
        const float behind = lit(sampleProbes(set, {0.3f, 0, 0}));
        check(behind < 0.05f, "wall: no leak through a wall, got " + text(behind));
        const float front = lit(sampleProbes(set, {-0.95f, 0, 0}));
        check(front > 0.9f, "wall: the visible side still sees its probe, got " + text(front));
    }
    // Overlapping volumes: the higher priority wins; equal priorities, the smaller.
    {
        auto big = grid({2, 2, 2}, {4, 4, 4}, [](uint32_t, uint32_t, uint32_t) { return 0.2f; });
        auto small = grid({2, 2, 2}, {1, 1, 1}, [](uint32_t, uint32_t, uint32_t) { return 0.8f; });
        set.volumes = {big, small};
        check(std::abs(lit(sampleProbes(set, {0, 0, 0})) - 0.8f) < 1e-3f, "priority: equal, the smaller volume wins");
        set.volumes[0].priority = 5;
        check(std::abs(lit(sampleProbes(set, {0, 0, 0})) - 0.2f) < 1e-3f, "priority: higher wins");
        check(std::abs(lit(sampleProbes(set, {3, 0, 0})) - 0.2f) < 1e-3f, "priority: only the big one here");
    }
    // Boundary blending: full inside, fading to the environment at the face.
    {
        auto v = grid({2, 2, 2}, {2, 2, 2}, [](uint32_t, uint32_t, uint32_t) { return 1.0f; });
        v.blendDistance = 1.0f;
        set.volumes = {v};
        check(sampleProbes(set, {0, 0, 0}).coverage == 1.0f, "blend: deep inside, full");
        const ProbeSample edge = sampleProbes(set, {1.5f, 0, 0});
        check(std::abs(edge.coverage - 0.5f) < 1e-3f, "blend: halfway through the blend, half");
        check(std::abs(lit(edge) - (0.5f + 0.5f * 0.05f)) < 1e-3f, "blend: the rest is the environment");
        check(sampleProbes(set, {1.999f, 0, 0}).coverage < 0.01f, "blend: nothing at the face");
    }
    // Rotated and moved volumes follow their transform.
    {
        auto v = grid({2, 2, 2}, {2, 0.5f, 0.5f}, [](uint32_t, uint32_t, uint32_t) { return 0.7f; });
        v.toWorld = glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0)) *
                    glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 1, 0));
        set.volumes = {v};
        check(sampleProbes(set, {10, 0, 1.8f}).coverage == 1.0f, "rotated: covers along its turned long axis");
        check(sampleProbes(set, {11.8f, 0, 0}).coverage == 0.0f, "rotated: not along the world axis");
        check(
            std::abs(v.probePosition(1, 0, 0).z - (-2.0f)) < 1e-4f ||
                std::abs(v.probePosition(1, 0, 0).z - 2.0f) < 1e-4f,
            "rotated: probe positions turn with it");
    }
    // A doorway: a bright room and a dark room joined through a gap. Walking from one to
    // the other changes smoothly, without a black frame or a jump.
    {
        auto v = grid({5, 1, 1}, {2, 1, 1}, [](uint32_t x, uint32_t, uint32_t) {
            return x < 2 ? 1.0f : x == 2 ? 0.5f : 0.02f;
        });
        set.volumes = {v};
        float previous = lit(sampleProbes(set, {-2, 0, 0})), worstStep = 0.0f, darkest = 1.0f;
        for (int i = 1; i <= 200; ++i) {
            const ProbeSample s = sampleProbes(set, {-2.0f + 0.02f * float(i), 0, 0});
            const float value = lit(s);
            worstStep = std::max(worstStep, std::abs(value - previous));
            darkest = std::min(darkest, value);
            previous = value;
        }
        check(worstStep < 0.02f, "doorway: no sudden change, largest step " + text(worstStep));
        check(darkest >= 0.019f, "doorway: never black");
    }
    // Invalid payloads.
    {
        std::vector<uint8_t> bytes;
        writeLightingProbes(set, bytes);
        LightingProbeSet back;
        std::string error;
        check(readLightingProbes(bytes, back, error), "payload: reads");
        bytes[0] = 'X';
        check(!readLightingProbes(bytes, back, error), "payload: wrong magic refused");
        LightingProbeSet tooMany;
        LightingProbeVolume huge;
        huge.counts = {1000, 1000, 1};
        tooMany.volumes = {huge};
        check(!writeLightingProbes(tooMany, bytes), "payload: a grid without its probes isn't written");
    }

    if (failures) {
        std::fprintf(stderr, "%d diffuse probe check(s) failed\n", failures);
        return 1;
    }
    std::printf("diffuse probes: all checks passed\n");
    return 0;
}
