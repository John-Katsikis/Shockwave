// Colour, exposure and fog contracts (A05, A06; docs/Graphics/04-ColorAndAtmosphere.md and
// chapter 7): white balance, .cube LUTs, metering and adaptation, and the fog closed forms,
// checked against hand-worked values. No GPU.
#include <Renderer/PostProcess.h>

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/packing.hpp>

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
bool near(float a, float b, float tolerance = 1e-5f) { return std::abs(a - b) <= tolerance; }
bool near(glm::vec3 a, glm::vec3 b, float tolerance = 1e-5f) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
}

void testWhiteBalance() {
    // D65 to D65 is the identity.
    const glm::mat3 identity = whiteBalanceMatrix(kD65White);
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) check(near(identity[c][r], c == r ? 1.0f : 0.0f), "D65 is the identity");
    // The matrices invert each other, and white (1, 1, 1) is D65 with Y = 1.
    const glm::vec3 white = linearSrgbToXyz() * glm::vec3(1.0f);
    check(
        near(white.y, 1.0f, 1e-4f) && near(white.x / (white.x + white.y + white.z), kD65White.x, 1e-4f),
        "sRGB white is D65");
    check(near(xyzToLinearSrgb() * white, glm::vec3(1.0f), 1e-4f), "the matrices round-trip");
    // Neutralizing: a white surface under illuminant A (incandescent, x 0.4476 y 0.4074),
    // which looks orange in sRGB, comes out neutral.
    const glm::vec2 a(0.4476f, 0.4074f);
    const glm::vec3 xyzA(a.x / a.y, 1.0f, (1.0f - a.x - a.y) / a.y);
    const glm::vec3 orange = xyzToLinearSrgb() * xyzA;
    check(orange.r > orange.b * 2.0f, "illuminant A looks orange in sRGB");
    const glm::vec3 neutral = whiteBalanceMatrix(a) * orange;
    check(near(neutral.r, neutral.g, 2e-3f) && near(neutral.g, neutral.b, 2e-3f), "illuminant A neutralized");
    check(near(neutral.g, 1.0f, 2e-2f), "keeps the brightness (Y = 1)");
    // Refused whites.
    std::string error;
    check(!validWhitePoint({0.0f, 0.3f}, &error) && !error.empty(), "x = 0 refused");
    check(!validWhitePoint({0.6f, 0.4f}), "x + y = 1 refused");
    check(!validWhitePoint({0.7f, 0.29f}), "a deep red white refused (gain limit)");
    check(validWhitePoint(a), "illuminant A accepted");
    const glm::mat3 refused = whiteBalanceMatrix({0.7f, 0.29f});
    check(near(refused * glm::vec3(0.2f, 0.5f, 0.9f), glm::vec3(0.2f, 0.5f, 0.9f)), "a refused white changes nothing");
}

void testLuts() {
    std::string error;
    // A neutral table maps every colour to itself, either encoding.
    const Lut neutral = neutralLut(17);
    check(neutral.table.size() == 17 * 17 * 17, "neutral has size^3 entries");
    for (glm::vec3 c :
         {glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(0.18f, 0.5f, 0.9f), glm::vec3(0.003f, 0.7f, 0.31f)}) {
        check(near(sampleLut(neutral, c, LutEncoding::Linear), c, 1e-5f), "neutral linear is the identity");
        check(near(sampleLut(neutral, c, LutEncoding::Srgb), c, 1e-5f), "neutral sRGB is the identity");
    }
    // Axis order: red fastest. A table whose output is (r, 0, 0) only along red's axis.
    Lut axes = neutralLut(17);
    for (glm::vec3& v : axes.table) v = glm::vec3(v.x, v.y * 0.5f, v.z * 0.25f);
    const glm::vec3 ramp = sampleLut(axes, glm::vec3(0.75f, 0.75f, 0.75f), LutEncoding::Linear);
    check(near(ramp, glm::vec3(0.75f, 0.375f, 0.1875f), 1e-5f), "red, green and blue are on their own axes");
    check(
        near(axes.at(16, 0, 0), glm::vec3(1.0f, 0.0f, 0.0f)) && near(axes.at(0, 0, 16), glm::vec3(0.0f, 0.0f, 0.25f)),
        "entry (r, g, b) is index r + g size + b size^2");
    // Endpoints clamp, never wrap.
    check(
        near(sampleLut(axes, glm::vec3(2.0f, -1.0f, 0.0f), LutEncoding::Linear), glm::vec3(1.0f, 0.0f, 0.0f)),
        "outside the domain clamps");
    // Round trip through text.
    Lut parsed;
    check(parseCubeLut(writeCubeLut(axes), parsed, error), "a written table parses: " + error);
    check(parsed.size == 17 && near(parsed.at(5, 9, 13), axes.at(5, 9, 13)), "and reads back the same");
    // Only 17, 33 and 65 entries a side.
    check(
        !parseCubeLut("LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n", parsed, error) &&
            error.find("17, 33 or 65") != std::string::npos,
        "unsupported sizes refused");
    check(!parseCubeLut("LUT_1D_SIZE 17\n", parsed, error) && error.find("line 1") == 0, "1D tables refused");
    check(
        !parseCubeLut("TITLE \"x\"\nLUT_3D_SIZE 17\n0 0 0\n", parsed, error) &&
            error.find("1 rows") != std::string::npos,
        "too few rows refused");
    Lut flat = neutral;
    flat.domainMin.x = 1.0f;
    check(!parseCubeLut(writeCubeLut(flat), parsed, error), "a domain of no width refused");
    check(!parseCubeLut("LUT_3D_SIZE 17\n0 0\n", parsed, error) && error.find("line 2") == 0, "short rows refused");
    check(!parseCubeLut("SOMETHING 1\n", parsed, error), "unknown keywords refused");
    // The GPU atlas: entry (r, g, b) at texel (r + b size, g).
    const TextureData t = lutTexture(axes);
    check(t.width == 17 * 17 && t.height == 17 && t.format == TextureFormat::RGBA16F && t.valid(), "atlas shape");
    // A table over 0..2 is resampled onto 0..1: the atlas holds what sampleLut gives there.
    Lut wide = neutralLut(17);
    wide.domainMax = glm::vec3(2.0f);
    const TextureData w = lutTexture(wide);
    auto texel = [&](const TextureData& data, uint32_t r, uint32_t g, uint32_t b) {
        const auto* p =
            reinterpret_cast<const uint16_t*>(data.bytes.data()) + (size_t(g) * data.width + r + b * 17) * 4;
        return glm::vec3(glm::unpackHalf1x16(p[0]), glm::unpackHalf1x16(p[1]), glm::unpackHalf1x16(p[2]));
    };
    const glm::vec3 expected = sampleLut(wide, glm::vec3(1.0f, 0.5f, 0.25f), LutEncoding::Linear);
    check(
        glm::all(glm::epsilonEqual(texel(w, 16, 8, 4), expected, 2e-3f)) && std::abs(expected.x - 0.5f) < 1e-3f,
        "other domains resampled onto 0..1");
    check(glm::all(glm::epsilonEqual(texel(t, 16, 0, 0), axes.at(16, 0, 0), 2e-3f)), "unit domains copied");
}

void testExposure() {
    AutoExposureDesc desc;
    std::string error;
    check(validAutoExposure(desc, &error), "defaults are valid");
    AutoExposureDesc bad = desc;
    bad.minEv = 3;
    bad.maxEv = 1;
    check(!validAutoExposure(bad), "min above max refused");
    bad = desc;
    bad.lowPercentile = 0.9f;
    bad.highPercentile = 0.5f;
    check(!validAutoExposure(bad), "unordered percentiles refused");

    uint32_t bin = 0;
    bool clamped = false;
    check(!histogramBin(std::numeric_limits<float>::quiet_NaN(), bin), "NaN ignored");
    check(!histogramBin(-1.0f, bin), "negative ignored");
    check(!histogramBin(std::numeric_limits<float>::infinity(), bin), "infinity ignored");
    check(histogramBin(0.0f, bin, &clamped) && bin == 0 && clamped, "black clamps to the floor");
    check(histogramBin(1e9f, bin, &clamped) && bin == kExposureBins - 1 && clamped, "very bright clamps to the top");
    check(histogramBin(1.0f, bin, &clamped) && !clamped && near(histogramBinLog2(bin), 0.0f, 0.12f), "1.0 is log2 0");

    // Zero weight: no target (keep the last exposure).
    std::vector<uint32_t> histogram(kExposureBins, 0);
    float target = 99.0f;
    check(!meteredExposure(histogram, desc, target) && target == 99.0f, "an empty histogram meters nothing");
    // Everything at luminance 0.18: no exposure change (log2(0.18) - log2(0.18)).
    histogramBin(0.18f, bin);
    histogram[bin] = 1000;
    check(
        meteredExposure(histogram, desc, target) && near(target, std::log2(0.18f) - histogramBinLog2(bin)),
        "a mid-grey scene keeps exposure");
    // Everything at 4.0 (two stops over 1): about log2(0.18) - 2.
    histogram.assign(kExposureBins, 0);
    histogramBin(4.0f, bin);
    histogram[bin] = 1000;
    float mean = 0;
    check(meteredExposure(histogram, desc, target, &mean) && near(mean, 2.0f, 0.12f), "bright scene meters 2 stops");
    // A muzzle flash: 3% of the screen very bright is trimmed off by the 95th percentile.
    histogram.assign(kExposureBins, 0);
    uint32_t dim = 0, flash = 0;
    histogramBin(0.18f, dim);
    histogramBin(5000.0f, flash);
    histogram[dim] = 970;
    histogram[flash] = 30;
    float withFlash = 0, without = 0;
    meteredExposure(histogram, desc, withFlash);
    histogram[flash] = 0;
    meteredExposure(histogram, desc, without);
    check(near(withFlash, without, 1e-4f), "a small flash doesn't move exposure");
    // Limits.
    desc.maxEv = 1.0f;
    histogram.assign(kExposureBins, 0);
    histogram[0] = 10;   // pitch black
    check(meteredExposure(histogram, desc, target) && target == 1.0f, "max_ev limits a dark scene");
    desc = {};
    // Center weighting: middle 1, corners 1/8.
    check(meteringWeight(ExposureMetering::Average, {0.0f, 0.0f}) == 256u, "average weighs corners fully");
    check(meteringWeight(ExposureMetering::Center, {0.5f, 0.5f}) == 256u, "center weighs the middle fully");
    check(meteringWeight(ExposureMetering::Center, {0.0f, 0.0f}) == 32u, "and corners an eighth");

    // Adaptation: the same result for 30, 60 and 120 Hz toward a fixed target (7.11).
    float evs[3];
    const int rates[3] = {30, 60, 120};
    for (int k = 0; k < 3; ++k) {
        float ev = 0.0f;
        for (int i = 0; i < rates[k]; ++i) ev = adaptExposure(ev, -3.0f, 1.0f / float(rates[k]), desc);
        evs[k] = ev;
    }
    // One second of darkening (tau 1 s): -3 (1 - 1/e).
    check(near(evs[0], -3.0f * (1.0f - std::exp(-1.0f)), 1e-4f), "darkening follows exp(-t / tau)");
    check(near(evs[0], evs[1], 1e-4f) && near(evs[1], evs[2], 1e-4f), "frame rate doesn't change adaptation");
    // Brightening uses its own time: 2 s.
    float up = 0.0f;
    for (int i = 0; i < 60; ++i) up = adaptExposure(up, 3.0f, 1.0f / 60.0f, desc);
    check(near(up, 3.0f * (1.0f - std::exp(-0.5f)), 1e-4f), "brightening takes brighten_time");
    // A long pause doesn't snap.
    check(adaptExposure(0.0f, -3.0f, 10.0f, desc) > -1.0f, "a pause is clamped to a quarter second");
    desc.darkenTime = 0.0f;
    check(adaptExposure(0.0f, -3.0f, 0.001f, desc) == -3.0f, "a time of 0 jumps");
}

void testFog() {
    FogDesc fog;
    check(!fog.active(), "no density, no fog");
    check(analyticFog(fog, {}, {0, 0, -1}, 100.0f, glm::vec3(1.0f)).transmittance == 1.0f, "zero density: identity");
    // 7.7's reference: sigma 0.1, 10 m, T = e^-1, and fog radiance 1 over black is 1 - e^-1.
    fog.density = 0.1f;
    const FogSegment s = analyticFog(fog, {0, 5, 0}, {0, 0, -1}, 10.0f, glm::vec3(1.0f));
    check(near(s.transmittance, 0.3678794412f, 1e-6f), "T = exp(-1)");
    check(near(fogged(s, glm::vec3(0.0f)).x, 0.6321205588f, 1e-6f), "black behind: 1 - exp(-1)");
    // Partition invariance: 1, 2, 16 and 64 homogeneous segments agree with one.
    for (int n : {1, 2, 16, 64}) {
        FogSegment sum;
        for (int i = 0; i < n; ++i) sum = combineFog(sum, fogSegment(0.1f, glm::vec3(0.1f), 10.0f / float(n)));
        check(
            near(sum.transmittance, s.transmittance, 1e-5f) && near(sum.scattering.x, s.scattering.x, 1e-5f),
            "segments: " + std::to_string(n));
    }
    // sigma 0: S = q ds.
    const FogSegment clear = fogSegment(0.0f, glm::vec3(2.0f), 3.0f);
    check(clear.transmittance == 1.0f && near(clear.scattering.x, 6.0f), "no extinction: q ds");
    // Height fog: horizontal ray limit tau = sigma(origin) L, no NaN.
    fog.heightFalloff = 0.5f;
    fog.baseHeight = 0.0f;
    const float horizontal = fogOpticalDepth(fog, 2.0f, 0.0f, 20.0f);
    check(near(horizontal, 0.1f * std::exp(-1.0f) * 20.0f, 1e-5f), "horizontal: sigma at the origin times L");
    check(std::isfinite(fogOpticalDepth(fog, 2.0f, 1e-9f, 20.0f)), "nearly horizontal stays finite");
    check(near(fogOpticalDepth(fog, 2.0f, 1e-6f, 20.0f), horizontal, 1e-5f), "and tends to the limit");
    // Going down into thicker fog: positive, larger than going up.
    const float down = fogOpticalDepth(fog, 2.0f, -0.7071f, 20.0f), upward = fogOpticalDepth(fog, 2.0f, 0.7071f, 20.0f);
    check(down > horizontal && upward < horizontal && upward > 0.0f, "denser below, thinner above, never negative");
    // Closed form against a fine Riemann sum (max density out of the way).
    fog.maxDensity = 100.0f;
    const float deep = fogOpticalDepth(fog, 2.0f, -0.7071f, 8.0f);
    double sum = 0.0;
    const int steps = 200000;
    for (int i = 0; i < steps; ++i) {
        const double t = (i + 0.5) * 8.0 / steps;
        sum += fogDensityAt(fog, float(2.0 - 0.7071 * t)) * 8.0 / steps;
    }
    check(
        near(deep, float(sum), 2e-3f * float(sum)),
        "height closed form matches the integral: " + std::to_string(deep) + " vs " + std::to_string(sum));
    fog.maxDensity = 1.0f;
    // Bounded: a deep pit can't overflow.
    check(fogOpticalDepth(fog, -1000.0f, -1.0f, 1e6f) <= kMaxOpticalDepth, "optical depth is bounded");
    check(fogOpticalDepth(fog, 2.0f, -1.0f, 1e5f) <= fog.maxDensity * 1e5f, "and by max density");
    // The start distance: nothing before it.
    fog.heightFalloff = 0.0f;
    fog.start = 5.0f;
    check(analyticFog(fog, {}, {0, 0, -1}, 4.0f, glm::vec3(1.0f)).transmittance == 1.0f, "before fog_start: clear");
    check(
        near(analyticFog(fog, {}, {0, 0, -1}, 15.0f, glm::vec3(1.0f)).transmittance, 0.3678794412f, 1e-6f),
        "after it, only the part past the start");
    // Invalid input.
    FogDesc bad;
    bad.density = -0.1f;
    check(!validFog(bad), "negative extinction refused");
    bad = {};
    bad.anisotropy = 0.95f;
    check(!validFog(bad), "anisotropy past 0.9 refused");
}

void testPhase() {
    // Normalised over the sphere for several g: 2 pi integral of p(mu) dmu = 1.
    for (float g : {-0.8f, -0.3f, 0.0f, 0.3f, 0.8f}) {
        double sum = 0.0;
        const int steps = 20000;
        for (int i = 0; i < steps; ++i) {
            const float mu = -1.0f + (float(i) + 0.5f) * 2.0f / float(steps);
            sum += henyeyGreenstein(g, mu) * 2.0 / steps;
        }
        check(near(float(sum * 2.0 * 3.14159265358979), 1.0f, 1e-3f), "phase normalised, g " + std::to_string(g));
    }
    check(near(henyeyGreenstein(0.0f, 0.3f), 1.0f / (4.0f * 3.14159265f)), "g 0 is isotropic");
    // g > 0: brightest when the light's propagation and the way to the camera agree (looking
    // toward the sun through the fog).
    check(henyeyGreenstein(0.5f, 1.0f) > henyeyGreenstein(0.5f, -1.0f) * 10.0f, "forward scattering lobe");
    check(henyeyGreenstein(0.99f, 1.0f) == henyeyGreenstein(0.9f, 1.0f), "g clamped to 0.9");
}

void testComposition() {
    // 7.10: alpha-over of S_p + T_p Lp on the fogged background keeps the front segment once.
    const FogSegment front{glm::vec3(0.3f), 0.6f}, back{glm::vec3(0.2f), 0.5f};
    const glm::vec3 lp(0.9f, 0.1f, 0.4f), lb(0.2f, 0.7f, 0.1f);
    const float alpha = 0.35f;
    const glm::vec3 background = fogged(combineFog(front, back), lb);
    const glm::vec3 blended = alpha * fogged(front, lp) + (1.0f - alpha) * background;
    const glm::vec3 expected =
        front.scattering + front.transmittance * (alpha * lp + (1.0f - alpha) * fogged(back, lb));
    check(near(blended, expected, 1e-6f), "alpha over fog: the front segment once");
    // Additive: T E, never another S.
    const glm::vec3 e(1.0f, 0.5f, 0.0f);
    check(near(background + front.transmittance * e - background, 0.6f * e, 1e-6f), "additive light only dims");
}

void testFroxels() {
    FroxelGrid low = froxelGrid(1920, 1080, FogQuality::Low, 0.1f, 64.0f);
    check(low.tileSize == 16 && low.tilesX == 120 && low.tilesY == 68 && low.slices == 48, "low at 1080p");
    FroxelGrid high = froxelGrid(1920, 1080, FogQuality::High, 0.1f, 64.0f);
    check(high.cells() <= kMaxFroxelCells, "high fits the cap");
    check(high.tileSize == 16, "high at 1080p doubles its tile to fit: " + std::to_string(high.tileSize));
    FroxelGrid medium = froxelGrid(1280, 720, FogQuality::Medium, 0.1f, 64.0f);
    check(medium.tileSize == 12 && medium.slices == 64 && medium.bytes() == uint64_t(medium.cells()) * 8, "medium");
    check(
        near(froxelSliceDistance(medium, 0.0f), 0.1f) && near(froxelSliceDistance(medium, 64.0f), 64.0f, 1e-3f),
        "slices span near to far");
    check(near(froxelSliceCoordinate(medium, froxelSliceDistance(medium, 17.5f)), 17.5f, 1e-3f), "and invert");
    check(froxelGrid(1280, 720, FogQuality::Analytic, 0.1f, 64.0f).cells() == 0, "analytic has no volume");
    check(froxelGrid(1280, 720, FogQuality::Off, 0.1f, 64.0f).cells() == 0, "nor does off");
}
}   // namespace

int main() {
    testWhiteBalance();
    testLuts();
    testExposure();
    testFog();
    testPhase();
    testComposition();
    testFroxels();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("postprocess: all passed\n");
    return 0;
}
