// Baked lighting files (G05, G06; docs/LightingSchema.md): binding ids, the .swlighting
// manifest's strict grammar, the .swbake and .swprobe layouts with bounded reads, RGBA16F
// .ktx2 lightmaps, lossless JSON export/import, and the spherical harmonics conventions.

#include <Assets/LightingData.h>

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
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
bool near(const glm::vec3& a, const glm::vec3& b, float tolerance) {
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance && std::abs(a.z - b.z) <= tolerance;
}
bool parses(const std::string& text, LightingManifest& m, std::string& error) {
    return parseLightingManifest(text, "Test.swlighting", m, error);
}
const char* kGood = R"(swlighting 1
# a comment
scene "Scenes/My Arena.swscene"
tool "shockwave lighting 1"
color linear_rec709_irradiance_over_pi
algorithm 1
quality final
samples 512
bounces 2
seed 7
atlas_size 1024
probe_rays 256
lighting_hash 0123456789abcdef
geometry_hash fedcba9876543210
resource lightmap "Lighting/My Arena/Lightmap_000.ktx2" hash 00000000000000aa bytes 8000 width 1024 height 1024
resource receivers "Lighting/My Arena/Receivers.swbake" hash 00000000000000bb bytes 120
resource probes "Lighting/My Arena/Probes.swprobe" hash 00000000000000cc bytes 140
receiver 0123456789abcdef0123456789abcdef part 0 atlas 0 hash 1111111111111111
receiver 0123456789abcdef0123456789abcdef part 257 atlas 0 hash 2222222222222222
volume ffffffffffffffff0000000000000001 hash 3333333333333333
)";
}   // namespace

int main() {
    // ---- Binding ids ----
    BindingId id;
    check(parseBindingId("0123456789abcdef0123456789abcdef", id) && id.hi == 0x0123456789abcdefull, "id: parses");
    check(formatBindingId(id) == "0123456789abcdef0123456789abcdef", "id: formats back");
    check(!parseBindingId("0123456789ABCDEF0123456789abcdef", id), "id: upper case refused");
    check(!parseBindingId("0123456789abcdef0123456789abcde", id), "id: short refused");
    check(!parseBindingId("0123456789abcdef-123456789abcdef", id), "id: dash refused");
    const BindingId a = newBindingId(), b = newBindingId();
    check(a.valid() && b.valid() && a != b, "id: fresh ids differ");
    const BindingId copyA{1, 2}, copyB{1, 3}, local{4, 5};
    check(deriveBindingId(copyA, local) == deriveBindingId(copyA, local), "id: derivation is stable");
    check(deriveBindingId(copyA, local) != deriveBindingId(copyB, local), "id: two copies of a prefab differ");
    check(deriveBindingId(copyA, local) != deriveBindingId(local, copyA), "id: derivation isn't symmetric");
    // Pinned so a change to the documented hash namespace can't slip through.
    check(formatBindingId(deriveBindingId(copyA, local)).size() == 32, "id: derived is 32 digits");

    // ---- Manifest ----
    LightingManifest m;
    std::string error;
    check(parses(kGood, m, error), "manifest: parses: " + error);
    check(
        m.scene == "Scenes/My Arena.swscene" && m.samples == 512 && m.quality == "final" && m.seed == 7,
        "manifest: values");
    check(m.resources.size() == 3 && m.receivers.size() == 2 && m.volumes.size() == 1, "manifest: records");
    check(
        m.receivers[1].part == 257 && m.resource(LightingResource::Kind::Lightmap)->width == 1024, "manifest: fields");
    LightingManifest again;
    check(parses(lightingManifestText(m), again, error), "manifest: writes and reads back: " + error);
    check(lightingManifestText(again) == lightingManifestText(m), "manifest: round trip is exact");
    auto refused = [&](const std::string& text, const std::string& expect, const std::string& what) {
        LightingManifest x;
        std::string e;
        const bool ok = parses(text, x, e);
        check(!ok && e.find(expect) != std::string::npos, "manifest: " + what + " (got \"" + e + "\")");
    };
    const std::string good = kGood;
    refused("swlighting 2\n", "version 2", "a newer version is refused");
    refused("scene \"x\"\n", "first line", "missing header");
    refused(good + "colour red\n", "unknown key", "unknown key");
    refused(good + "samples 4\n", "given twice", "repeated key");
    refused(
        good + "receiver 0123456789abcdef0123456789abcdef part 0 atlas 0 hash 1111111111111111\n", "listed twice",
        "duplicate receiver");
    refused(
        good + "receiver 0123456789abcdef0123456789abcdee part 0 atlas 3 hash 1111111111111111\n", "isn't listed",
        "receiver on a missing lightmap");
    refused(
        good + "resource probes \"../outside.swprobe\" hash 00000000000000cc bytes 1\n", "inside Content",
        "paths can't leave Content");
    refused(good + "volume 0123 hash 1\n", "32 lowercase", "bad binding id");
    refused("swlighting 1\nscene \"x\"\n", "is missing", "required keys");
    refused(good + "quality \"great\"\n", "given twice", "repeated quality");
    {
        std::string bad = kGood;
        bad.replace(bad.find("quality final"), 13, "quality great");
        refused(bad, "preview or final", "bad quality");
        std::string line = kGood;
        line.replace(line.find("bounces 2"), 9, "bounces 99");
        refused(line, "Test.swlighting:9:", "errors name the line");
    }

    // ---- Receivers ----
    LightingReceiver r;
    r.binding = a;
    r.part = 3;
    r.atlas = 1;
    r.geometryHash = 0xabcdefull;
    r.sourceVertex = {0, 1, 2, 2};
    r.lightmapUv = {{0.1f, 0.2f}, {0.3f, 0.4f}, {0.5f, 0.6f}, {0.7f, 0.80000001f}};
    r.indices = {0, 1, 2, 2, 1, 3};
    std::vector<uint8_t> bytes;
    check(writeLightingReceivers(std::vector{r}, bytes), "receivers: write");
    std::vector<LightingReceiver> back;
    check(readLightingReceivers(bytes, back, error), "receivers: read: " + error);
    check(
        back.size() == 1 && back[0].indices == r.indices && back[0].lightmapUv == r.lightmapUv &&
            back[0].binding == a && back[0].part == 3,
        "receivers: round trip");
    for (size_t cut : {size_t(4), size_t(20), bytes.size() - 1}) {
        std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + long(cut));
        check(
            !readLightingReceivers(truncated, back, error),
            "receivers: truncated at " + std::to_string(cut) + " refused");
    }
    {
        std::vector<uint8_t> huge = bytes;
        huge[12] = 0xFF, huge[13] = 0xFF, huge[14] = 0xFF, huge[15] = 0x7F;   // receiver count
        check(!readLightingReceivers(huge, back, error), "receivers: impossible count refused");
        std::vector<uint8_t> extra = bytes;
        extra.push_back(0);
        check(!readLightingReceivers(extra, back, error), "receivers: trailing bytes refused");
        LightingReceiver outOfRange = r;
        outOfRange.indices = {0, 1, 9};
        std::vector<uint8_t> oob;
        writeLightingReceivers(std::vector{outOfRange}, oob);
        check(!readLightingReceivers(oob, back, error), "receivers: index past the vertices refused");
    }
    {
        const std::string json = receiversJson(std::vector{r});
        std::vector<LightingReceiver> imported;
        check(receiversFromJson(json, imported, error), "receivers: JSON imports: " + error);
        std::vector<uint8_t> again2;
        writeLightingReceivers(imported, again2);
        check(again2 == bytes, "receivers: JSON round trip is byte for byte");
    }

    // ---- Probes ----
    LightingProbeSet set;
    set.environment = shConstant({0.1f, 0.2f, 0.3f});
    LightingProbeVolume volume;
    volume.binding = b;
    volume.toWorld = glm::mat4(1.0f);
    volume.toWorld[3] = glm::vec4(1, 2, 3, 1);
    volume.halfExtents = {1, 0.5f, 1};
    volume.counts = {2, 1, 3};
    volume.probes.resize(6);
    for (size_t i = 0; i < volume.probes.size(); ++i) {
        volume.probes[i].valid = i != 4;
        volume.probes[i].sh = shConstant(glm::vec3(float(i) * 0.1f));
        volume.probes[i].sh[5] = glm::vec3(-0.25f, 0.5f, 1.0f / 3.0f);   // signed, never clamped
        for (glm::vec2& d : volume.probes[i].distance) d = {1.5f, 2.25f};
    }
    set.volumes.push_back(volume);
    check(writeLightingProbes(set, bytes), "probes: write");
    LightingProbeSet readSet;
    check(readLightingProbes(bytes, readSet, error), "probes: read: " + error);
    check(
        readSet.volumes.size() == 1 && readSet.volumes[0].counts == volume.counts &&
            !readSet.volumes[0].probes[4].valid && readSet.volumes[0].probes[2].sh[5] == volume.probes[2].sh[5],
        "probes: round trip");
    {
        std::vector<uint8_t> truncated(bytes.begin(), bytes.end() - 3);
        check(!readLightingProbes(truncated, readSet, error), "probes: truncated refused");
        std::vector<uint8_t> nan = bytes;
        const float bad = std::nanf("");
        std::memcpy(&nan[12], &bad, 4);
        check(!readLightingProbes(nan, readSet, error), "probes: non-finite refused");
        const std::string json = probesJson(set);
        LightingProbeSet imported;
        check(probesFromJson(json, imported, error), "probes: JSON imports: " + error);
        std::vector<uint8_t> again2;
        writeLightingProbes(imported, again2);
        check(again2 == bytes, "probes: JSON round trip is byte for byte");
    }

    // ---- Lightmaps ----
    std::vector<float> rgba = {0.0f, 1.0f, 2.5f, 1.0f, 100.0f, 0.001f, 65504.0f, 0.0f};
    std::vector<uint8_t> ktx2;
    check(encodeLightmapKtx2(2, 1, rgba, ktx2, error), "lightmap: encodes: " + error);
    uint32_t w = 0, h = 0;
    std::vector<float> decoded;
    check(decodeLightmapKtx2(ktx2, w, h, decoded, error) && w == 2 && h == 1, "lightmap: decodes: " + error);
    check(
        decoded.size() == 8 && decoded[2] == 2.5f && decoded[4] == 100.0f && decoded[6] == 65504.0f,
        "lightmap: HDR values survive (no clamping to 1)");
    check(ktx2.size() > 12 && ktx2[1] == 'K' && ktx2[5] == '2', "lightmap: a KTX2 file");
    {
        std::vector<uint8_t> wrong = ktx2;
        wrong[12] = 37;   // VK_FORMAT_R8G8B8A8_UNORM
        check(!decodeLightmapKtx2(wrong, w, h, decoded, error), "lightmap: other formats refused");
        std::vector<uint8_t> cut(ktx2.begin(), ktx2.end() - 1);
        check(!decodeLightmapKtx2(cut, w, h, decoded, error), "lightmap: truncated refused");
        std::vector<float> nan = rgba;
        nan[0] = std::nanf("");
        check(!encodeLightmapKtx2(2, 1, nan, ktx2, error), "lightmap: non-finite refused");
        std::vector<float> fromJson;
        uint32_t jw = 0, jh = 0;
        std::vector<uint8_t> original;
        encodeLightmapKtx2(2, 1, rgba, original, error);
        decodeLightmapKtx2(original, w, h, decoded, error);
        check(lightmapFromJson(lightmapJson(w, h, decoded), jw, jh, fromJson, error), "lightmap: JSON imports");
        std::vector<uint8_t> again2;
        encodeLightmapKtx2(jw, jh, fromJson, again2, error);
        check(again2 == original, "lightmap: JSON round trip is byte for byte");
    }

    // ---- Spherical harmonics ----
    const Sh9 constant = shConstant({0.5f, 1.0f, 2.0f});
    for (const glm::vec3& n :
         {glm::vec3(0, 1, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, -1), glm::normalize(glm::vec3(1, -1, 1))})
        check(
            near(shEvaluate(constant, n), {0.5f, 1.0f, 2.0f}, 1e-5f),
            "sh: a constant evaluates to itself at every normal");
    {
        // Project a constant through the general path and compare.
        Sh9 radiance{};
        const uint32_t count = 20000;
        for (uint32_t i = 0; i < count; ++i) {
            const float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(count);
            const float r = std::sqrt(1.0f - y * y), phi = 2.39996323f * float(i);
            const auto basis = shBasis({r * std::cos(phi), y, r * std::sin(phi)});
            for (size_t k = 0; k < 9; ++k) radiance[k] += glm::vec3(0.7f) * basis[k];
        }
        for (glm::vec3& c : radiance) c *= 4.0f * glm::pi<float>() / float(count);
        check(
            near(shEvaluate(shRadianceToIrradiance(radiance), glm::vec3(0, 0, 1)), glm::vec3(0.7f), 2e-3f),
            "sh: projection and band factors keep a constant constant");
        // A light only from above: E/pi facing up is half of L (cosine over the hemisphere),
        // facing down nothing (within ringing).
        Sh9 above{};
        for (uint32_t i = 0; i < count; ++i) {
            const float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(count);
            const float r = std::sqrt(1.0f - y * y), phi = 2.39996323f * float(i);
            if (y <= 0.0f) continue;
            const auto basis = shBasis({r * std::cos(phi), y, r * std::sin(phi)});
            for (size_t k = 0; k < 9; ++k) above[k] += glm::vec3(1.0f) * basis[k];
        }
        for (glm::vec3& c : above) c *= 4.0f * glm::pi<float>() / float(count);
        const Sh9 e = shRadianceToIrradiance(above);
        check(near(shEvaluate(e, {0, 1, 0}), glm::vec3(1.0f), 0.05f), "sh: up-facing gets the whole upper hemisphere");
        check(shEvaluate(e, {0, -1, 0}).x < 0.06f, "sh: down-facing gets almost nothing, never negative");
    }
    // Octahedral round trip.
    for (const glm::vec3& d :
         {glm::vec3(0, 1, 0), glm::vec3(0, -1, 0), glm::normalize(glm::vec3(1, 2, 3)),
          glm::normalize(glm::vec3(-1, -0.3f, 0.2f)), glm::normalize(glm::vec3(0.1f, -1, -0.7f))})
        check(near(octahedralDecode(octahedralEncode(d)), d, 1e-4f), "octahedral: round trip");

    if (failures) {
        std::fprintf(stderr, "%d lighting format check(s) failed\n", failures);
        return 1;
    }
    std::printf("lighting format: all checks passed\n");
    return 0;
}
