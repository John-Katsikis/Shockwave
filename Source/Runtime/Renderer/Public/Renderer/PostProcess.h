#pragma once

// Colour, exposure and fog (A05, A06; docs/Graphics/04-ColorAndAtmosphere.md and chapter 7's
// contracts; the user guides are docs/PostProcess.md and docs/Fog.md).
//
// The renderer's half: what a frame is asked to do (PostProcessDesc, made by the Engine from
// PostProcessVolumes and .swpostprocess profiles), the player's settings that limit it, and
// the maths, written once on the CPU so tests can hold the shaders to it:
//
//   linear lighting and fog -> TAA -> metering (before bloom) -> bloom -> white balance ->
//   exposure -> the tone mapper -> creative LUTs (display-linear) -> FXAA -> UI -> sRGB
//
// Scene colour is linear Rec.709 / sRGB primaries with a D65 white. Nothing here changes a
// scene without volumes or fog: every default is the identity.

#include <Renderer/RenderTypes.h>
#include <Renderer/TextureData.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

// ---- White balance (4.3) ----
// The authored input is the chromaticity (CIE xy) of the illuminant white to neutralize:
// a surface lit by that white comes out neutral. D65 changes nothing.
inline constexpr glm::vec2 kD65White{0.3127f, 0.3290f};
// The largest per-channel cone gain an adaptation may apply; past it the white is refused.
inline constexpr float kMaxWhiteGain = 4.0f;
// x > 0, y > 0, x + y < 1 and within kMaxWhiteGain; error says why not.
bool validWhitePoint(glm::vec2 xy, std::string* error = nullptr);
// Column vectors (glm): xyz = linearSrgbToXyz() * rgb. IEC 61966-2-1 primaries, D65.
glm::mat3 linearSrgbToXyz();
glm::mat3 xyzToLinearSrgb();
// Bradford chromatic adaptation from the source white to D65, as one linear sRGB matrix:
// RGB' = M * RGB. The identity for D65, and for a white validWhitePoint refuses.
glm::mat3 whiteBalanceMatrix(glm::vec2 sourceWhite);

// ---- Creative LUTs (4.4) ----
// The engine's LUT domain is tone-mapped, display-linear RGB in [0, 1]. A table authored on
// sRGB-encoded values (most grading tools) is marked Srgb: the colour is encoded before the
// look-up and the result decoded after.
enum class LutEncoding : int { Srgb, Linear };
const char* lutEncodingName(LutEncoding encoding);
inline constexpr std::array<uint32_t, 3> kLutSizes{17, 33, 65};
inline constexpr uint32_t kMaxActiveLuts = 2;   // blended per frame; more are approximated
// A 3D table: size^3 entries, red changing fastest, then green, then blue (the .cube order).
struct Lut {
    std::string title;
    uint32_t size = 0;
    glm::vec3 domainMin{0.0f}, domainMax{1.0f};
    std::vector<glm::vec3> table;
    const glm::vec3& at(uint32_t r, uint32_t g, uint32_t b) const { return table[(b * size + g) * size + r]; }
};
// A text .cube file: TITLE (optional), LUT_3D_SIZE (17, 33 or 65), DOMAIN_MIN / DOMAIN_MAX
// (optional, 0 and 1 by default, min < max), then exactly size^3 rows of three numbers.
// 1D tables, LUT_1D_SIZE and anything else are refused rather than guessed at. Errors are
// "line N: what's wrong".
bool parseCubeLut(std::string_view text, Lut& lut, std::string& error);
std::string writeCubeLut(const Lut& lut);
Lut neutralLut(uint32_t size);
float srgbEncode(float linear);
float srgbDecode(float encoded);
// What tonemap.frag does with one table: trilinear, clamped to the domain, never repeated.
glm::vec3 sampleLut(const Lut& lut, glm::vec3 displayLinear, LutEncoding encoding);
// The table as the GPU samples it: a size*size by size RGBA16F atlas, entry (r, g, b) at
// texel (r + b * size, g), one level. Shaders/tonemap.frag reads it with two bilinear taps.
TextureData lutTexture(const Lut& lut);

// ---- Auto exposure (4.5) ----
// Exposure is in stops (EV) on top of the scene's light: colour * 2^EV goes to the tone
// mapper, the same as Environment.exposure.
enum class ExposureMetering : int { Average, Center };
const char* exposureMeteringName(ExposureMetering metering);
struct AutoExposureDesc {
    float compensation = 0.0f;   // stops added to the metered exposure
    float minEv = -8.0f, maxEv = 8.0f;   // the exposure it may choose, stops
    float targetGray = 0.18f;   // the metered average maps to this
    float lowPercentile = 0.5f, highPercentile = 0.95f;   // 0..1 of the weight, trimmed outside
    float brightenTime = 2.0f;   // seconds (time constant) to raise exposure, going into the dark
    float darkenTime = 1.0f;   // seconds to lower it, going into bright light
    ExposureMetering metering = ExposureMetering::Average;
    bool resetOnCut = true;   // jump straight to the target on a camera cut
};
// minEv <= maxEv, percentiles ordered in 0..1, times and target gray finite and positive.
bool validAutoExposure(const AutoExposureDesc& desc, std::string* error = nullptr);
// The histogram: kExposureBins bins of log2 luminance over [kHistogramMinLog2,
// kHistogramMaxLog2]; luminance below the floor or above the top lands in the end bins and
// counts as clamped. Rec.709 luminance weights.
inline constexpr uint32_t kExposureBins = 128;
inline constexpr float kHistogramMinLog2 = -12.0f, kHistogramMaxLog2 = 16.0f;
inline constexpr float kLuminanceFloor = 0.000244140625f;   // 2^-12
float exposureLuminance(glm::vec3 rgb);
// The bin a luminance lands in; false for NaN, infinity or a negative value (ignored).
bool histogramBin(float luminance, uint32_t& bin, bool* clamped = nullptr);
// The log2 luminance a bin stands for (its centre).
float histogramBinLog2(uint32_t bin);
// Center metering's weight for a pixel at uv (0..1): 1 in the middle, down to 1/8 at the
// corners; Average weighs every pixel 1. In 1/256ths, as the GPU adds them up.
uint32_t meteringWeight(ExposureMetering metering, glm::vec2 uv);
// The exposure to aim for: trims the low and high percentiles of the weighted histogram,
// averages log2 luminance over the rest, and returns log2(targetGray) - mean + compensation
// within minEv..maxEv. False when the histogram holds no weight (keep the last exposure).
bool meteredExposure(
    std::span<const uint32_t> histogram, const AutoExposureDesc& desc, float& targetEv, float* meanLog2 = nullptr);
// One step of adaptation in stop space: ev + (target - ev) * (1 - exp(-dt / tau)), tau the
// brighten or darken time by direction. dt is clamped to 0..kMaxAdaptationStep so a pause
// doesn't snap; a fixed target gives the same result for any split of the time (chapter 7.11).
inline constexpr float kMaxAdaptationStep = 0.25f;
float adaptExposure(float ev, float targetEv, float dt, const AutoExposureDesc& desc);

// ---- Fog (4.6 - 4.8) ----
// Off draws none; analytic is the closed form along each ray (sun lit, no shadows); low,
// medium and high add a froxel volume near the camera lit by every light, with the sun's
// shadows, at coarser to finer cells.
enum class FogQuality : int { Off, Analytic, Low, Medium, High };
const char* fogQualityName(FogQuality quality);
// A participating medium with extinction sigma(y) = density * exp(-heightFalloff * (y -
// baseHeight)), clamped to maxDensity, in 1/m. Light scattered toward the camera per metre is
// q = sigma * (color + albedo * sum(E_light * phase)); with only color, a dense fog fades to
// color. Albedo 0 is a fog that absorbs and only glows with its own color.
struct FogDesc {
    float density = 0.0f;   // 1/m at baseHeight; 0: no fog
    float heightFalloff = 0.0f;   // 1/m; 0 the same everywhere
    float baseHeight = 0.0f;   // m
    float maxDensity = 1.0f;   // 1/m, the most it may reach anywhere
    glm::vec3 color{0.5f, 0.6f, 0.7f};   // linear radiance of the light it scatters from the surroundings
    float albedo = 0.8f;   // 0..1: how much of the sun's and lamps' light it scatters
    float anisotropy = 0.3f;   // Henyey-Greenstein g, -0.9..0.9: above 0 glows toward the light
    float start = 0.0f;   // m from the camera before it begins
    float skyDistance = 1000.0f;   // m: how far away the sky counts as
    float volumetricDistance = 64.0f;   // m the froxel volume reaches; analytic past it
    bool active() const { return density > 0.0f && maxDensity > 0.0f; }
};
bool validFog(const FogDesc& fog, std::string* error = nullptr);
inline constexpr float kMaxFogAnisotropy = 0.9f;
inline constexpr float kMaxOpticalDepth = 64.0f;   // exp(-64): nothing gets through
// Henyey-Greenstein phase for mu = dot(incoming propagation, outgoing propagation toward the
// camera): (1 - g^2) / (4 pi (1 + g^2 - 2 g mu)^1.5). g is clamped to +-kMaxFogAnisotropy.
float henyeyGreenstein(float g, float mu);
// The fog's extinction at a height, clamped.
float fogDensityAt(const FogDesc& fog, float y);
// Optical depth from origin along a unit direction for length metres: the closed form of
// 7.8 with a series near the horizontal limit, bounded by maxDensity * length and
// kMaxOpticalDepth. Never negative.
float fogOpticalDepth(const FogDesc& fog, float originY, float directionY, float length);
// One homogeneous segment (7.7): T = exp(-sigma ds), S = q (1 - T) / sigma (q ds at sigma 0).
struct FogSegment {
    glm::vec3 scattering{0.0f};   // S: light added along it
    float transmittance = 1.0f;   // T
};
FogSegment fogSegment(float sigma, glm::vec3 source, float ds);
// Front to back: (S1 + T1 S2, T1 T2).
FogSegment combineFog(const FogSegment& front, const FogSegment& back);
// The analytic fog between the camera and a point distance metres along a unit ray, its
// start skipped, lit by `light` (color + albedo * the directional lights' scattering for
// this ray direction, which is constant along it).
FogSegment analyticFog(
    const FogDesc& fog, const glm::vec3& camera, const glm::vec3& direction, float distance, const glm::vec3& light);
// What fog does to what's behind it: an opaque surface's radiance L becomes S + T L; a
// straight-alpha surface's source is S + T Lp (blending over the fogged background keeps the
// front segment once, 7.10); additive emission E only dims, T E.
inline glm::vec3 fogged(const FogSegment& fog, const glm::vec3& radiance) {
    return fog.scattering + fog.transmittance * radiance;
}

// The froxel volume (4.7): screen tiles by logarithmic depth slices from the near plane to
// volumetricDistance. Low 16 px x 48, medium 12 px x 64, high 8 px x 64; the tile doubles until
// it fits kMaxFroxelCells. Each cell holds the fog integrated from the camera to its far
// side, as half floats (rgb scattering, transmittance): 8 bytes.
inline constexpr uint32_t kMaxFroxelCells = 1u << 20;
struct FroxelGrid {
    uint32_t tileSize = 0;   // pixels
    uint32_t tilesX = 0, tilesY = 0, slices = 0;
    float nearPlane = 0, farPlane = 0;   // metres
    uint32_t cells() const { return tilesX * tilesY * slices; }
    uint64_t bytes() const { return uint64_t(cells()) * 8u; }
};
FroxelGrid froxelGrid(uint32_t width, uint32_t height, FogQuality quality, float nearPlane, float farPlane);
// The view distance at slice boundary k (0 the near plane, slices the far plane).
float froxelSliceDistance(const FroxelGrid& grid, float k);
// The continuous slice coordinate of a view distance (inverse of the above).
float froxelSliceCoordinate(const FroxelGrid& grid, float distance);

// ---- What a frame is asked for, and the player's limits ----
struct PostProcessLut {
    TextureHandle texture{};   // lutTexture
    uint32_t size = 0;
    LutEncoding encoding = LutEncoding::Srgb;
    float weight = 0.0f;   // of the graded result; what's left keeps the ungraded colour
};
struct PostProcessDesc {
    // Without an override the Environment's exposure and bloom (LightingDesc) apply.
    bool overrideExposure = false;
    float exposure = 0.0f;   // stops
    float autoExposure = 0.0f;   // 0 the manual exposure .. 1 fully metered, blended in stops
    AutoExposureDesc autoParams;
    glm::vec2 whitePoint = kD65White;
    std::array<PostProcessLut, kMaxActiveLuts> luts{};
    uint32_t lutCount = 0;
    bool lutsApproximated = false;   // more LUTs were active than kMaxActiveLuts
    bool overrideBloom = false;
    float bloomIntensity = 0.04f;
    FogDesc fog;
    float deltaTime = 1.0f / 60.0f;   // seconds since the last frame (adaptation)
};
// The player's settings. Each one off is exactly the scene without it.
struct PostProcessSettings {
    bool colorGrading = true;   // white balance and LUTs
    bool autoExposure = true;   // off keeps every profile's manual exposure
    FogQuality fog = FogQuality::Medium;
    bool operator==(const PostProcessSettings&) const = default;
};
// The last frame's (Renderer::postProcessStats).
struct PostProcessStats {
    bool whiteBalance = false;   // a white other than D65 applied
    uint32_t luts = 0;   // tables blended
    bool lutsApproximated = false;
    float exposure = 0.0f;   // stops used, a frame or two late when metered
    float autoExposure = 0.0f;   // share metered
    float meteredTarget = 0.0f;   // the histogram's target, stops (GPU, late)
    float clampedFraction = 0.0f;   // of metered weight outside the histogram's range
    bool meteredValid = false;
    uint32_t exposureResets = 0;
    FogQuality fogRequested = FogQuality::Off;
    FogQuality fogQuality = FogQuality::Off;   // as drawn
    const char* fogReason = "";   // why it differs
    FroxelGrid froxels;
    uint64_t fogBytes = 0, exposureBytes = 0;
};

}   // namespace shockwave
