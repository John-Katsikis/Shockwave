#include <Renderer/PostProcess.h>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace shockwave {

namespace {

constexpr float kPi = 3.14159265358979f;

// From rows, as printed in the standards, to glm's columns.
glm::mat3 rows(float a, float b, float c, float d, float e, float f, float g, float h, float i) {
    return glm::transpose(glm::mat3(a, b, c, d, e, f, g, h, i));
}

const glm::mat3& bradford() {
    static const glm::mat3 m = rows(0.8951f, 0.2664f, -0.1614f, -0.7502f, 1.7135f, 0.0367f, 0.0389f, -0.0685f, 1.0296f);
    return m;
}

glm::vec3 whiteXyz(glm::vec2 xy) { return {xy.x / xy.y, 1.0f, (1.0f - xy.x - xy.y) / xy.y}; }

bool coneGain(glm::vec2 xy, glm::vec3& gain, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (!std::isfinite(xy.x) || !std::isfinite(xy.y) || xy.x <= 0.0f || xy.y <= 0.0f || xy.x + xy.y >= 1.0f)
        return fail("a white point needs x > 0, y > 0 and x + y < 1");
    const glm::vec3 source = bradford() * whiteXyz(xy);
    const glm::vec3 target = bradford() * whiteXyz(kD65White);
    for (int i = 0; i < 3; ++i) {
        if (!(source[i] > 1e-6f)) return fail("that white point has no light in one of the eye's cone responses");
        gain[i] = target[i] / source[i];
        if (!(gain[i] <= kMaxWhiteGain && gain[i] >= 1.0f / kMaxWhiteGain))
            return fail("that white point is too far from daylight to neutralize (over 4x in one channel)");
    }
    return true;
}

// Strict number parsing for .cube files.
bool parseFloat(std::string_view text, float& out) {
    if (text.empty()) return false;
    const char* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc() && ptr == end && std::isfinite(out);
}

std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) break;
        size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
        out.push_back(line.substr(i, j - i));
        i = j;
    }
    return out;
}

}   // namespace

// ---- White balance ----

bool validWhitePoint(glm::vec2 xy, std::string* error) {
    glm::vec3 gain;
    return coneGain(xy, gain, error);
}

glm::mat3 linearSrgbToXyz() {
    return rows(
        0.4124564f, 0.3575761f, 0.1804375f, 0.2126729f, 0.7151522f, 0.0721750f, 0.0193339f, 0.1191920f, 0.9503041f);
}

glm::mat3 xyzToLinearSrgb() { return glm::inverse(linearSrgbToXyz()); }

glm::mat3 whiteBalanceMatrix(glm::vec2 sourceWhite) {
    glm::vec3 gain;
    if (!coneGain(sourceWhite, gain, nullptr)) return glm::mat3(1.0f);
    if (std::abs(sourceWhite.x - kD65White.x) < 1e-7f && std::abs(sourceWhite.y - kD65White.y) < 1e-7f)
        return glm::mat3(1.0f);
    glm::mat3 scale(1.0f);
    for (int i = 0; i < 3; ++i) scale[i][i] = gain[i];
    const glm::mat3 adapt = glm::inverse(bradford()) * scale * bradford();
    return xyzToLinearSrgb() * adapt * linearSrgbToXyz();
}

// ---- LUTs ----

const char* lutEncodingName(LutEncoding encoding) { return encoding == LutEncoding::Linear ? "linear" : "srgb"; }

bool parseCubeLut(std::string_view text, Lut& lut, std::string& error) {
    lut = {};
    bool haveSize = false, haveMin = false, haveMax = false;
    size_t expected = 0;
    int lineNumber = 0;
    auto fail = [&](const std::string& why) {
        error = "line " + std::to_string(lineNumber) + ": " + why;
        return false;
    };
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t end = std::min(text.find('\n', pos), text.size());
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto w = words(line);
        if (w.empty() || w[0][0] == '#') {
            if (end == text.size()) break;
            continue;
        }
        const std::string_view key = w[0];
        if (key == "TITLE") {
            if (!lut.table.empty()) return fail("TITLE after the table");
            const size_t open = line.find('"'), close = line.rfind('"');
            if (open == std::string_view::npos || close == open) return fail("TITLE needs a quoted name");
            lut.title = std::string(line.substr(open + 1, close - open - 1));
        } else if (key == "LUT_3D_SIZE") {
            if (haveSize) return fail("LUT_3D_SIZE given twice");
            if (!lut.table.empty()) return fail("LUT_3D_SIZE after the table");
            uint32_t size = 0;
            if (w.size() != 2) return fail("LUT_3D_SIZE needs one number");
            auto [p, ec] = std::from_chars(w[1].data(), w[1].data() + w[1].size(), size);
            if (ec != std::errc() || p != w[1].data() + w[1].size()) return fail("LUT_3D_SIZE needs a whole number");
            if (std::find(kLutSizes.begin(), kLutSizes.end(), size) == kLutSizes.end())
                return fail("LUT_3D_SIZE " + std::to_string(size) + " isn't supported: use 17, 33 or 65");
            lut.size = size;
            expected = size_t(size) * size * size;
            lut.table.reserve(expected);
            haveSize = true;
        } else if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX") {
            const bool isMin = key == "DOMAIN_MIN";
            if ((isMin && haveMin) || (!isMin && haveMax)) return fail(std::string(key) + " given twice");
            if (!lut.table.empty()) return fail(std::string(key) + " after the table");
            glm::vec3 v;
            if (w.size() != 4 || !parseFloat(w[1], v.x) || !parseFloat(w[2], v.y) || !parseFloat(w[3], v.z))
                return fail(std::string(key) + " needs three numbers");
            (isMin ? lut.domainMin : lut.domainMax) = v;
            (isMin ? haveMin : haveMax) = true;
        } else if (key == "LUT_1D_SIZE" || key == "LUT_1D_INPUT_RANGE" || key == "LUT_3D_INPUT_RANGE") {
            return fail(std::string(key) + ": only 3D tables with DOMAIN_MIN and DOMAIN_MAX are supported");
        } else if ((key[0] >= '0' && key[0] <= '9') || key[0] == '-' || key[0] == '.' || key[0] == '+') {
            if (!haveSize) return fail("table rows before LUT_3D_SIZE");
            glm::vec3 v;
            if (w.size() != 3 || !parseFloat(w[0], v.x) || !parseFloat(w[1], v.y) || !parseFloat(w[2], v.z))
                return fail("a table row needs three numbers");
            if (lut.table.size() == expected) return fail("more than LUT_3D_SIZE^3 rows");
            lut.table.push_back(v);
        } else {
            return fail("unknown keyword " + std::string(key));
        }
        if (end == text.size()) break;
    }
    if (!haveSize) {
        error = "no LUT_3D_SIZE (only 3D .cube tables are supported)";
        return false;
    }
    if (lut.table.size() != expected) {
        error = "the table has " + std::to_string(lut.table.size()) + " rows; LUT_3D_SIZE " + std::to_string(lut.size) +
                " needs " + std::to_string(expected);
        return false;
    }
    for (int i = 0; i < 3; ++i)
        if (!(lut.domainMin[i] < lut.domainMax[i])) {
            error = "DOMAIN_MIN must be below DOMAIN_MAX in every channel";
            return false;
        }
    return true;
}

std::string writeCubeLut(const Lut& lut) {
    std::string out;
    char line[128];
    if (!lut.title.empty()) out += "TITLE \"" + lut.title + "\"\n";
    out += "LUT_3D_SIZE " + std::to_string(lut.size) + "\n";
    std::snprintf(line, sizeof(line), "DOMAIN_MIN %.9g %.9g %.9g\n", lut.domainMin.x, lut.domainMin.y, lut.domainMin.z);
    out += line;
    std::snprintf(line, sizeof(line), "DOMAIN_MAX %.9g %.9g %.9g\n", lut.domainMax.x, lut.domainMax.y, lut.domainMax.z);
    out += line;
    for (const glm::vec3& v : lut.table) {
        std::snprintf(line, sizeof(line), "%.9g %.9g %.9g\n", v.x, v.y, v.z);
        out += line;
    }
    return out;
}

Lut neutralLut(uint32_t size) {
    Lut lut;
    lut.title = "Shockwave neutral " + std::to_string(size);
    lut.size = size;
    lut.table.reserve(size_t(size) * size * size);
    const float scale = size > 1 ? 1.0f / float(size - 1) : 0.0f;
    for (uint32_t b = 0; b < size; ++b)
        for (uint32_t g = 0; g < size; ++g)
            for (uint32_t r = 0; r < size; ++r)
                lut.table.emplace_back(float(r) * scale, float(g) * scale, float(b) * scale);
    return lut;
}

float srgbEncode(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

float srgbDecode(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

glm::vec3 sampleLut(const Lut& lut, glm::vec3 c, LutEncoding encoding) {
    if (lut.size < 2 || lut.table.size() != size_t(lut.size) * lut.size * lut.size) return c;
    if (encoding == LutEncoding::Srgb) c = {srgbEncode(c.x), srgbEncode(c.y), srgbEncode(c.z)};
    const float last = float(lut.size - 1);
    glm::vec3 p;
    for (int i = 0; i < 3; ++i)
        p[i] = std::clamp((c[i] - lut.domainMin[i]) / (lut.domainMax[i] - lut.domainMin[i]), 0.0f, 1.0f) * last;
    const glm::uvec3 i0 = glm::min(glm::uvec3(glm::floor(p)), glm::uvec3(lut.size - 1));
    const glm::uvec3 i1 = glm::min(i0 + 1u, glm::uvec3(lut.size - 1));
    const glm::vec3 f = p - glm::vec3(i0);
    auto lerpRg = [&](uint32_t b) {
        const glm::vec3 a = glm::mix(lut.at(i0.x, i0.y, b), lut.at(i1.x, i0.y, b), f.x);
        const glm::vec3 d = glm::mix(lut.at(i0.x, i1.y, b), lut.at(i1.x, i1.y, b), f.x);
        return glm::mix(a, d, f.y);
    };
    glm::vec3 out = glm::mix(lerpRg(i0.z), lerpRg(i1.z), f.z);
    if (encoding == LutEncoding::Srgb) out = {srgbDecode(out.x), srgbDecode(out.y), srgbDecode(out.z)};
    return out;
}

TextureData lutTexture(const Lut& lut) {
    TextureData t;
    t.format = TextureFormat::RGBA16F;
    t.srgb = false;
    t.width = lut.size * lut.size;
    t.height = lut.size;
    const size_t bytes = textureLevelBytes(t.format, t.width, t.height);
    t.levels.push_back({t.width, t.height, 0, bytes});
    t.bytes.resize(bytes);
    auto* out = reinterpret_cast<uint16_t*>(t.bytes.data());
    // The shader looks up 0..1: a table over another domain is resampled onto that, so
    // inputs outside its domain clamp to its edge as they do here.
    const bool unitDomain = lut.domainMin == glm::vec3(0.0f) && lut.domainMax == glm::vec3(1.0f);
    const float last = float(lut.size - 1);
    for (uint32_t b = 0; b < lut.size; ++b)
        for (uint32_t g = 0; g < lut.size; ++g)
            for (uint32_t r = 0; r < lut.size; ++r) {
                const glm::vec3 v =
                    unitDomain ? lut.at(r, g, b)
                               : sampleLut(lut, glm::vec3(float(r), float(g), float(b)) / last, LutEncoding::Linear);
                uint16_t* texel = out + (size_t(g) * t.width + r + size_t(b) * lut.size) * 4;
                texel[0] = glm::packHalf1x16(v.x);
                texel[1] = glm::packHalf1x16(v.y);
                texel[2] = glm::packHalf1x16(v.z);
                texel[3] = glm::packHalf1x16(1.0f);
            }
    return t;
}

// ---- Auto exposure ----

const char* exposureMeteringName(ExposureMetering metering) {
    return metering == ExposureMetering::Center ? "center" : "average";
}

bool validAutoExposure(const AutoExposureDesc& d, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    const float values[] = {d.compensation,  d.minEv,          d.maxEv,        d.targetGray,
                            d.lowPercentile, d.highPercentile, d.brightenTime, d.darkenTime};
    for (float v : values)
        if (!std::isfinite(v)) return fail("auto exposure values must be finite numbers");
    if (d.minEv > d.maxEv) return fail("auto_min_ev must not be above auto_max_ev");
    if (!(d.lowPercentile >= 0.0f && d.lowPercentile <= d.highPercentile && d.highPercentile <= 1.0f))
        return fail("auto exposure percentiles need 0 <= low <= high <= 1");
    if (!(d.targetGray > 0.0f)) return fail("auto_target_gray must be above 0");
    if (d.brightenTime < 0.0f || d.darkenTime < 0.0f) return fail("adaptation times can't be negative");
    return true;
}

float exposureLuminance(glm::vec3 rgb) { return glm::dot(rgb, glm::vec3(0.2126f, 0.7152f, 0.0722f)); }

bool histogramBin(float luminance, uint32_t& bin, bool* clamped) {
    if (!std::isfinite(luminance) || luminance < 0.0f) return false;
    const float l = std::log2(std::max(luminance, kLuminanceFloor));
    const float t = (l - kHistogramMinLog2) / (kHistogramMaxLog2 - kHistogramMinLog2);
    if (clamped) *clamped = luminance < kLuminanceFloor || t >= 1.0f;
    bin = uint32_t(std::clamp(t * float(kExposureBins), 0.0f, float(kExposureBins - 1)));
    return true;
}

float histogramBinLog2(uint32_t bin) {
    return kHistogramMinLog2 + (float(bin) + 0.5f) * (kHistogramMaxLog2 - kHistogramMinLog2) / float(kExposureBins);
}

uint32_t meteringWeight(ExposureMetering metering, glm::vec2 uv) {
    if (metering == ExposureMetering::Average) return 256u;
    const glm::vec2 d = (uv - 0.5f) * 2.0f;   // -1..1 across the screen
    const float r = std::min(glm::length(d) / 1.41421356f, 1.0f);
    const float w = 1.0f - 0.875f * r * r * (3.0f - 2.0f * r);   // smoothstep: 1 centre, 1/8 corners
    return uint32_t(w * 256.0f + 0.5f);
}

bool meteredExposure(
    std::span<const uint32_t> histogram, const AutoExposureDesc& desc, float& targetEv, float* meanLog2) {
    double total = 0.0;
    for (uint32_t h : histogram) total += h;
    if (!(total > 0.0)) return false;
    const double low = total * std::clamp(desc.lowPercentile, 0.0f, 1.0f);
    const double high = total * std::clamp(desc.highPercentile, 0.0f, 1.0f);
    double sum = 0.0, weight = 0.0, cumulative = 0.0;
    for (uint32_t i = 0; i < histogram.size(); ++i) {
        const double a = cumulative, b = cumulative + histogram[i];
        cumulative = b;
        const double overlap = std::min(b, high) - std::max(a, low);
        if (overlap > 0.0) {
            sum += overlap * histogramBinLog2(i);
            weight += overlap;
        }
    }
    if (!(weight > 0.0)) {
        // An empty trimmed range (low == high): the bin at that percentile.
        cumulative = 0.0;
        uint32_t i = 0;
        for (; i + 1 < histogram.size(); ++i) {
            cumulative += histogram[i];
            if (cumulative > low) break;
        }
        sum = histogramBinLog2(i);
        weight = 1.0;
    }
    const float mean = float(sum / weight);
    if (meanLog2) *meanLog2 = mean;
    targetEv = std::clamp(std::log2(desc.targetGray) - mean + desc.compensation, desc.minEv, desc.maxEv);
    return true;
}

float adaptExposure(float ev, float targetEv, float dt, const AutoExposureDesc& desc) {
    dt = std::clamp(std::isfinite(dt) ? dt : 0.0f, 0.0f, kMaxAdaptationStep);
    const float tau = targetEv > ev ? desc.brightenTime : desc.darkenTime;
    if (!(tau > 0.0f)) return targetEv;
    return ev + (targetEv - ev) * (1.0f - std::exp(-dt / tau));
}

// ---- Fog ----

const char* fogQualityName(FogQuality quality) {
    switch (quality) {
    case FogQuality::Off: return "off";
    case FogQuality::Analytic: return "analytic";
    case FogQuality::Low: return "low";
    case FogQuality::Medium: return "medium";
    case FogQuality::High: return "high";
    }
    return "off";
}

bool validFog(const FogDesc& f, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    const float values[] = {f.density,    f.heightFalloff, f.baseHeight,  f.maxDensity,
                            f.color.x,    f.color.y,       f.color.z,     f.albedo,
                            f.anisotropy, f.start,         f.skyDistance, f.volumetricDistance};
    for (float v : values)
        if (!std::isfinite(v)) return fail("fog values must be finite numbers");
    if (f.density < 0.0f || f.maxDensity < 0.0f) return fail("fog density can't be negative: it would add light");
    if (f.color.x < 0.0f || f.color.y < 0.0f || f.color.z < 0.0f) return fail("fog_color can't be negative");
    if (f.albedo < 0.0f || f.albedo > 1.0f) return fail("fog_albedo must be 0..1");
    if (std::abs(f.anisotropy) > kMaxFogAnisotropy) return fail("fog_anisotropy must be -0.9..0.9");
    if (f.start < 0.0f || f.skyDistance <= 0.0f || f.volumetricDistance <= 0.0f)
        return fail("fog distances must be positive (fog_start may be 0)");
    return true;
}

float henyeyGreenstein(float g, float mu) {
    g = std::clamp(g, -kMaxFogAnisotropy, kMaxFogAnisotropy);
    mu = std::clamp(mu, -1.0f, 1.0f);
    const float denom = 1.0f + g * g - 2.0f * g * mu;
    return (1.0f - g * g) / (4.0f * kPi * denom * std::sqrt(denom));
}

float fogDensityAt(const FogDesc& fog, float y) {
    if (!fog.active()) return 0.0f;
    const float e = std::clamp(-fog.heightFalloff * (y - fog.baseHeight), -30.0f, 30.0f);
    return std::min(fog.density * std::exp(e), fog.maxDensity);
}

float fogOpticalDepth(const FogDesc& fog, float originY, float directionY, float length) {
    if (!fog.active() || !(length > 0.0f)) return 0.0f;
    const float e = std::clamp(-fog.heightFalloff * (originY - fog.baseHeight), -30.0f, 30.0f);
    const float sigma0 = fog.density * std::exp(e);
    const float x = std::max(fog.heightFalloff * directionY * length, -30.0f);
    // (1 - e^-x) / x, with its series near 0 (the horizontal limit is 1).
    const float ratio = std::abs(x) < 1e-3f ? 1.0f - x * (0.5f - x * (1.0f / 6.0f - x / 24.0f)) : -std::expm1(-x) / x;
    const float tau = sigma0 * length * ratio;
    return std::clamp(std::min(tau, fog.maxDensity * length), 0.0f, kMaxOpticalDepth);
}

FogSegment fogSegment(float sigma, glm::vec3 source, float ds) {
    FogSegment s;
    const float x = std::max(sigma, 0.0f) * std::max(ds, 0.0f);
    s.transmittance = std::exp(-x);
    // q (1 - T) / sigma = q ds (1 - e^-x) / x.
    const float ratio = x < 1e-3f ? 1.0f - x * (0.5f - x / 6.0f) : -std::expm1(-x) / x;
    s.scattering = source * (std::max(ds, 0.0f) * ratio);
    return s;
}

FogSegment combineFog(const FogSegment& front, const FogSegment& back) {
    return {front.scattering + front.transmittance * back.scattering, front.transmittance * back.transmittance};
}

FogSegment analyticFog(
    const FogDesc& fog, const glm::vec3& camera, const glm::vec3& direction, float distance, const glm::vec3& light) {
    FogSegment s;
    if (!fog.active() || !(distance > fog.start)) return s;
    const glm::vec3 origin = camera + direction * fog.start;
    const float tau = fogOpticalDepth(fog, origin.y, direction.y, distance - fog.start);
    s.transmittance = std::exp(-tau);
    s.scattering = light * (1.0f - s.transmittance);
    return s;
}

FroxelGrid froxelGrid(uint32_t width, uint32_t height, FogQuality quality, float nearPlane, float farPlane) {
    FroxelGrid grid;
    if (quality < FogQuality::Low || width == 0 || height == 0) return grid;
    grid.tileSize = quality == FogQuality::Low ? 16u : quality == FogQuality::Medium ? 12u : 8u;
    grid.slices = quality == FogQuality::Low ? 48u : 64u;
    grid.nearPlane = std::max(nearPlane, 0.05f);
    grid.farPlane = std::max(farPlane, grid.nearPlane * 2.0f);
    for (;;) {
        grid.tilesX = (width + grid.tileSize - 1) / grid.tileSize;
        grid.tilesY = (height + grid.tileSize - 1) / grid.tileSize;
        if (grid.cells() <= kMaxFroxelCells) break;
        grid.tileSize *= 2;
    }
    return grid;
}

float froxelSliceDistance(const FroxelGrid& grid, float k) {
    return grid.nearPlane * std::pow(grid.farPlane / grid.nearPlane, k / float(grid.slices));
}

float froxelSliceCoordinate(const FroxelGrid& grid, float distance) {
    return float(grid.slices) * std::log(std::max(distance, 1e-6f) / grid.nearPlane) /
           std::log(grid.farPlane / grid.nearPlane);
}

}   // namespace shockwave
