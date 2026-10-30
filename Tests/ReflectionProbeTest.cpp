// Local reflection probes (G07; docs/GraphicsImplementation.md 11): cube face directions,
// box projection, weights at the faces, which probes a draw picks, how two blend over the
// global environment, the cube .ktx2 and its errors, capture resampling, the prefilter, and
// the .swreflection manifest. No GPU: these run anywhere.

#include <Assets/LightingData.h>
#include <Assets/ReflectionData.h>
#include <Renderer/ReflectionProbes.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
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
bool near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) {
    return glm::all(glm::lessThan(glm::abs(a - b), glm::vec3(eps)));
}

ReflectionProbeDesc box(glm::vec3 centre, glm::vec3 half, int32_t priority = 0, uint64_t order = 0) {
    ReflectionProbeDesc p;
    p.boxToWorld = glm::translate(glm::mat4(1.0f), centre);
    p.halfExtents = half;
    p.capturePosition = centre;
    p.blendDistance = 0.5f;
    p.priority = priority;
    p.order = order;
    return p;
}

// A cube whose every texel of face f is colour[f] at every level.
ReflectionCube faceColours(uint32_t size) {
    static const glm::vec3 colour[6] = {{1, 0, 0}, {0.2f, 0, 0}, {0, 1, 0}, {0, 0.2f, 0}, {0, 0, 1}, {0, 0, 0.2f}};
    ReflectionCube c;
    c.size = size;
    c.levels.resize(kReflectionLevels);
    for (uint32_t l = 0; l < kReflectionLevels; ++l) {
        const uint32_t s = c.levelSize(l);
        for (uint32_t f = 0; f < 6; ++f)
            for (uint32_t i = 0; i < s * s; ++i)
                c.levels[l].insert(c.levels[l].end(), {colour[f].r, colour[f].g, colour[f].b, 1.0f});
    }
    return c;
}
}   // namespace

int main() {
    // ---- Faces: every texel direction lands back on its own face and place ----
    {
        bool ok = true;
        for (uint32_t f = 0; f < 6; ++f)
            for (float s : {-0.9f, -0.3f, 0.0f, 0.5f, 0.95f})
                for (float t : {-0.8f, 0.0f, 0.4f, 0.9f}) {
                    glm::vec2 st;
                    const uint32_t back = cubeFaceOf(cubeFaceDirection(f, {s, t}), st);
                    ok = ok && back == f && std::abs(st.x - s) < 1e-5f && std::abs(st.y - t) < 1e-5f;
                }
        check(ok, "faces: cubeFaceOf inverts cubeFaceDirection");
        glm::vec2 st;
        check(cubeFaceOf({0, 0, 0}, st) == 0 && st == glm::vec2(0.0f), "faces: zero direction is face 0 centre");
        // The same orientation as the global environment: +Y's top edge looks toward -Z.
        check(cubeFaceDirection(2, {0, -1}).z < 0.0f, "faces: +Y top edge toward -Z");
        check(cubeFaceDirection(0, {0, -1}).y > 0.0f, "faces: +X top edge is up");
    }

    // ---- Box projection ----
    {
        const ReflectionProbeDesc p = box({0, 0, 0}, {4, 2, 4});
        glm::vec3 lookup;
        check(
            reflectionBoxProject(p, {0, 0, 0}, {1, 0, 0}, lookup) && near(glm::normalize(lookup), {1, 0, 0}),
            "box: from the capture point, the lookup is the ray");
        // From off-centre, along +X: the exit is (4, 0, 2), seen from the centre.
        check(
            reflectionBoxProject(p, {0, 0, 2}, {1, 0, 0}, lookup) && near(lookup, {4, 0, 2}),
            "box: off-centre exit seen from the capture point");
        // Rays parallel to two pairs of faces still find the third.
        check(
            reflectionBoxProject(p, {1, 1, 1}, {0, 0, -3}, lookup) && near(lookup, {1, 1, -4}),
            "box: parallel faces skipped, scale of R doesn't matter");
        check(!reflectionBoxProject(p, {5, 0, 0}, {1, 0, 0}, lookup), "box: outside the box fails");
        check(!reflectionBoxProject(p, {0, 0, 0}, {0, 0, 0}, lookup), "box: zero ray fails");
        // A box turned 90 degrees about Y: its long x side now runs along world z.
        ReflectionProbeDesc r = box({10, 0, 0}, {6, 2, 1});
        r.boxToWorld = glm::rotate(glm::translate(glm::mat4(1.0f), {10, 0, 0}), glm::radians(90.0f), {0, 1, 0});
        check(
            reflectionBoxProject(r, {10, 0, 0}, {0, 0, 1}, lookup) && near(lookup, {0, 0, 6}, 1e-3f),
            "box: rotated box's long side along world z");
        check(reflectionBoxProject(r, {10, 0, 3}, {0, 0, 1}, lookup), "box: inside the rotated box along z");
        check(!reflectionBoxProject(r, {13, 0, 0}, {1, 0, 0}, lookup), "box: outside the rotated box along x");
    }

    // ---- Weights ----
    {
        const ReflectionProbeDesc p = box({0, 0, 0}, {4, 2, 4});
        check(reflectionProbeWeight(p, {0, 0, 0}) == 1.0f, "weight: 1 deep inside");
        check(std::abs(reflectionProbeWeight(p, {0, 1.75f, 0}) - 0.5f) < 1e-5f, "weight: halfway through the fade");
        check(reflectionProbeWeight(p, {0, 2.0f, 0}) == 0.0f, "weight: 0 at the face");
        check(reflectionProbeWeight(p, {0, 3.0f, 0}) == 0.0f, "weight: 0 outside");
        ReflectionProbeDesc hard = p;
        hard.blendDistance = 0.0f;
        check(reflectionProbeWeight(hard, {0, 1.99f, 0}) == 1.0f, "weight: zero blend is a hard edge");
        // Monotonic toward the face: no step larger than the fade allows.
        float last = 1.0f, worst = 0.0f;
        for (float y = 1.0f; y <= 2.0f; y += 0.01f) {
            const float w = reflectionProbeWeight(p, {0, y, 0});
            check(w <= last + 1e-6f, "weight: never grows toward the face");
            worst = std::max(worst, last - w);
            last = w;
        }
        check(worst < 0.021f, "weight: smooth fade");
    }

    // ---- Selection ----
    {
        const ReflectionProbeDesc probes[] = {
            box({0, 0, 0}, {10, 10, 10}, 0, 1),   // large hall
            box({0, 0, 0}, {2, 2, 2}, 0, 2),   // small room inside it
            box({0, 0, 0}, {5, 5, 5}, 1, 3),   // higher priority
            box({50, 0, 0}, {2, 2, 2}, 5, 4),   // far away
        };
        const ReflectionSelection s = selectReflectionProbes(probes, {0, 0, 0, 1});
        check(s.count == 2 && s.index[0] == 2 && s.index[1] == 1, "select: priority first, then the smaller box");
        const ReflectionSelection none = selectReflectionProbes(probes, {30, 0, 0, 1});
        check(none.count == 0, "select: nothing touches, global only");
        const ReflectionSelection edge = selectReflectionProbes(probes, {52.5f, 0, 0, 1.0f});
        check(edge.count == 1 && edge.index[0] == 3, "select: a sphere touching the box counts");
        // Equal priority and size: the lower order first, whatever order they're given in.
        const ReflectionProbeDesc same[] = {box({0, 0, 0}, {2, 2, 2}, 0, 9), box({0, 0, 0}, {2, 2, 2}, 0, 3)};
        const ReflectionSelection tie = selectReflectionProbes(same, {0, 0, 0, 1});
        check(tie.count == 2 && tie.index[0] == 1, "select: ties by order");
    }

    // ---- Blending ----
    {
        const ReflectionProbeDesc equal[] = {box({0, 0, 0}, {2, 2, 2}), box({3, 0, 0}, {2, 2, 2})};
        ReflectionSelection s;
        s.count = 2;
        s.index = {0, 1};
        ReflectionBlend b = blendReflectionProbes(equal, s, {0.5f, 0.5f});
        check(
            std::abs(b.share[0] - 0.5f) < 1e-6f && std::abs(b.coverage - 1.0f) < 1e-6f,
            "blend: equal halves cover fully");
        b = blendReflectionProbes(equal, s, {0.2f, 0.2f});
        check(
            std::abs(b.share[0] - 0.2f) < 1e-6f && std::abs(b.coverage - 0.4f) < 1e-6f,
            "blend: partial coverage keeps the global for the rest");
        b = blendReflectionProbes(equal, s, {1.0f, 1.0f});
        check(std::abs(b.share[0] - 0.5f) < 1e-6f && b.coverage <= 1.0f, "blend: never past 1");
        b = blendReflectionProbes(equal, s, {0.0f, 0.0f});
        check(b.coverage == 0.0f, "blend: both failed, global only");
        const ReflectionProbeDesc ranked[] = {box({0, 0, 0}, {2, 2, 2}, 2), box({0, 0, 0}, {8, 8, 8}, 0)};
        b = blendReflectionProbes(ranked, s, {0.75f, 1.0f});
        check(
            std::abs(b.share[0] - 0.75f) < 1e-6f && std::abs(b.share[1] - 0.25f) < 1e-6f,
            "blend: higher priority goes over the lower");
        ReflectionSelection one;
        one.count = 1;
        b = blendReflectionProbes(equal, one, {0.3f, 0.0f});
        check(std::abs(b.coverage - 0.3f) < 1e-6f, "blend: one probe");
        b = blendReflectionProbes(equal, ReflectionSelection{}, {1.0f, 1.0f});
        check(b.coverage == 0.0f, "blend: none selected");
    }

    // ---- Cube file ----
    {
        ReflectionCube cube = faceColours(64);
        check(cube.valid(), "cube: valid");
        std::vector<uint8_t> bytes;
        std::string error;
        check(encodeReflectionKtx2(cube, bytes, error), "ktx2: encode " + error);
        check(isReflectionKtx2(bytes), "ktx2: recognised as a cube");
        ReflectionCube back;
        check(
            decodeReflectionKtx2(bytes, back, error) && back.size == 64 && back.levels.size() == kReflectionLevels,
            "ktx2: decode " + error);
        float worst = 0.0f;
        for (uint32_t l = 0; l < kReflectionLevels; ++l)
            for (size_t i = 0; i < cube.levels[l].size(); ++i)
                worst = std::max(worst, std::abs(back.levels[l][i] - cube.levels[l][i]));
        check(worst < 1e-3f, "ktx2: values round to half floats");
        const auto means = reflectionFaceMeans(back);
        check(near(means[0], {1, 0, 0}) && near(means[3], {0, 0.2f, 0}, 1e-3f), "ktx2: faces stay in order");
        std::vector<uint8_t> again;
        encodeReflectionKtx2(back, again, error);
        check(again == bytes, "ktx2: deterministic bytes");

        std::vector<uint8_t> bad = bytes;
        bad[36] = 1;   // faceCount
        check(
            !decodeReflectionKtx2(bad, back, error) && error.find("6 faces") != std::string::npos,
            "ktx2: wrong face count is an error");
        check(!isReflectionKtx2(bad), "ktx2: a 2D file isn't a cube");
        bad = bytes;
        bad.resize(bytes.size() - 100);
        check(!decodeReflectionKtx2(bad, back, error), "ktx2: truncated is an error");
        bad = bytes;
        bad[40] = 3;   // levelCount
        check(!decodeReflectionKtx2(bad, back, error), "ktx2: wrong level count is an error");
        ReflectionCube odd = faceColours(64);
        odd.size = 100;
        check(!encodeReflectionKtx2(odd, bytes, error), "ktx2: refuses a size it doesn't allow");

        // JSON round trip to the same bytes.
        encodeReflectionKtx2(cube, bytes, error);
        ReflectionCube fromJson;
        check(reflectionCubeFromJson(reflectionCubeJson(cube), fromJson, error), "json: import " + error);
        encodeReflectionKtx2(fromJson, again, error);
        check(again == bytes, "json: round trip gives the same .ktx2");

        const TextureData data = reflectionTextureData(cube);
        check(data.shape == TextureShape::Cube && data.faces() == 6 && data.valid(), "cube: as renderer texture data");
    }

    // ---- Sampling and the prefilter ----
    {
        ReflectionCube cube = faceColours(64);
        check(near(sampleReflectionCube(cube, {1, 0.1f, -0.1f}, 0.0f), {1, 0, 0}), "sample: +X face");
        check(near(sampleReflectionCube(cube, {0, 0, -1}, 0.0f), {0, 0, 0.2f}), "sample: -Z face");

        // A uniform cube stays uniform at every roughness.
        ReflectionCube flat = faceColours(64);
        for (auto& level : flat.levels)
            for (size_t i = 0; i < level.size(); i += 4) level[i] = level[i + 1] = level[i + 2] = 0.7f;
        prefilterReflectionCube(flat);
        bool uniform = true;
        for (uint32_t l = 0; l < kReflectionLevels; ++l)
            for (const glm::vec3& m : reflectionFaceMeans(flat, l))
                uniform = uniform && near(m, glm::vec3(0.7f), 2e-3f);
        check(uniform, "prefilter: a uniform cube stays uniform");

        ReflectionCube a = faceColours(64), b = faceColours(64);
        prefilterReflectionCube(a);
        prefilterReflectionCube(b);
        check(a.levels == b.levels, "prefilter: deterministic");
        check(a.levels[0] == faceColours(64).levels[0], "prefilter: level 0 untouched");
        // Rougher levels see more of the neighbouring faces: +X's red falls toward the mean.
        float last = 2.0f;
        bool falling = true;
        for (uint32_t l = 0; l < kReflectionLevels; ++l) {
            const float red = reflectionFaceMeans(a, l)[0].r;
            falling = falling && red <= last + 1e-4f;
            last = red;
        }
        check(falling && last < 0.9f, "prefilter: rougher levels blur the faces together");
    }

    // ---- Capture resampling: a fake capture of each face reads back as that face ----
    {
        bool ok = true;
        for (uint32_t f = 0; f < 6; ++f) {
            glm::mat4 view, projection;
            reflectionFaceCamera(f, {1, 2, 3}, 0.05f, 500.0f, view, projection);
            const glm::mat4 vp = projection * view;
            // Render: each pixel's colour is its view direction, encoded.
            SceneCapture capture;
            capture.width = 96;
            capture.height = 64;   // a stretched target: the camera still sees the whole face
            const glm::mat4 inv = glm::inverse(vp);
            for (uint32_t y = 0; y < capture.height; ++y)
                for (uint32_t x = 0; x < capture.width; ++x) {
                    const glm::vec2 ndc((x + 0.5f) / capture.width * 2 - 1, 1 - (y + 0.5f) / capture.height * 2);
                    const glm::vec4 a = inv * glm::vec4(ndc, 1.0f, 1.0f), b = inv * glm::vec4(ndc, 0.5f, 1.0f);
                    const glm::vec3 d = glm::normalize(glm::vec3(b) / b.w - glm::vec3(a) / a.w);
                    capture.rgba.insert(capture.rgba.end(), {d.x, d.y, d.z, 1.0f});
                }
            std::vector<float> face;
            std::string error;
            if (!resampleReflectionFace(capture, f, vp, 16, face, error)) {
                ok = false;
                continue;
            }
            for (uint32_t t = 0; t < 16; ++t)
                for (uint32_t s = 0; s < 16; ++s) {
                    const glm::vec3 want =
                        glm::normalize(cubeFaceDirection(f, {(s + 0.5f) / 8 - 1, (t + 0.5f) / 8 - 1}));
                    const float* got = &face[(t * 16 + s) * 4];
                    ok = ok && glm::dot(want, glm::normalize(glm::vec3(got[0], got[1], got[2]))) > 0.995f;
                }
        }
        check(ok, "capture: each face camera and resample agree with cubeFaceDirection");
        SceneCapture empty;
        std::vector<float> face;
        std::string error;
        glm::mat4 view, projection;
        reflectionFaceCamera(0, {}, 0.05f, 500.0f, view, projection);
        check(!resampleReflectionFace(empty, 0, projection * view, 16, face, error), "capture: empty is an error");
    }

    // ---- Manifest ----
    {
        ReflectionManifest m;
        m.scene = "Scenes/Hall.swscene";
        m.sceneHash = 0x1234;
        m.tool = "shockwave reflections 1";
        ReflectionProbeRecord r;
        r.binding = newBindingId();
        r.path = "Lighting/Hall_reflections/Probe_x.ktx2";
        r.hash = 77;
        r.bytes = 1000;
        r.size = 128;
        r.probeHash = 5;
        m.probes.push_back(r);
        const std::string text = reflectionManifestText(m);
        ReflectionManifest back;
        std::string error;
        check(parseReflectionManifest(text, "x.swreflection", back, error), "manifest: parse " + error);
        check(
            back.scene == m.scene && back.sceneHash == m.sceneHash && back.probes.size() == 1 &&
                back.probes[0].binding == r.binding && back.probes[0].probeHash == 5 && back.probe(r.binding),
            "manifest: round trip");
        check(reflectionManifestText(back) == text, "manifest: text is stable");
        check(
            !parseReflectionManifest(text + "bogus 1\n", "x.swreflection", back, error) &&
                error.find("x.swreflection:") != std::string::npos,
            "manifest: unknown key is an error with file:line");
        check(
            !parseReflectionManifest(text + text.substr(text.find("probe ")), "x.swreflection", back, error),
            "manifest: repeated probe is an error");
        check(!parseReflectionManifest("swreflection 2\n", "x.swreflection", back, error), "manifest: other version");
        check(!reflectionManifestSchema().empty(), "manifest: schema text");
    }

    check(validReflectionResolution(128) && !validReflectionResolution(100), "resolutions");
    check(reflectionCubeBytes(128) > 1000000 && reflectionCubeBytes(128) < 1200000, "budget: 128 is about 1 MiB");

    if (failures) {
        std::fprintf(stderr, "%d reflection probe checks failed\n", failures);
        return 1;
    }
    std::printf("reflection probe checks passed\n");
    return 0;
}
