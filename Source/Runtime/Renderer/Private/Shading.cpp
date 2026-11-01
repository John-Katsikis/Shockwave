// Character and specialized shading maths (Renderer/Shading.h). Must match mesh.frag and
// skin_blur.comp.
#include <Renderer/Shading.h>

#include <algorithm>
#include <cmath>

namespace shockwave {

namespace {
constexpr float kPi = 3.14159265358979f;
}

const char* shadingFamilyName(ShadingFamily family) {
    switch (family) {
    case ShadingFamily::Standard: return "standard";
    case ShadingFamily::Skin: return "skin";
    case ShadingFamily::HairCard: return "hair_card";
    case ShadingFamily::Transmission: return "transmission";
    }
    return "standard";
}

const char* transmissionQualityName(TransmissionQuality quality) {
    switch (quality) {
    case TransmissionQuality::Off: return "off";
    case TransmissionQuality::Low: return "low";
    case TransmissionQuality::Medium: return "medium";
    case TransmissionQuality::High: return "high";
    }
    return "off";
}

const char* skinQualityName(SkinQuality quality) {
    switch (quality) {
    case SkinQuality::Off: return "off";
    case SkinQuality::Low: return "low";
    case SkinQuality::High: return "high";
    }
    return "off";
}

const char* hairQualityName(HairQuality quality) { return quality == HairQuality::High ? "high" : "low"; }

uint32_t skinBlurTaps(SkinQuality quality) {
    return quality == SkinQuality::High ? 13u : quality == SkinQuality::Low ? 7u : 0u;
}

float schlickFresnel(float f0, float cosTheta) {
    const float m = 1.0f - std::clamp(cosTheta, 0.0f, 1.0f);
    return f0 + (1.0f - f0) * m * m * m * m * m;
}

float coatBaseWeight(float coat, float nDotL, float nDotV) {
    const float c = std::clamp(coat, 0.0f, 1.0f);
    return (1.0f - c * schlickFresnel(kCoatF0, nDotL)) * (1.0f - c * schlickFresnel(kCoatF0, nDotV));
}

float dielectricFresnel(float cosI, float etaI, float etaT) {
    cosI = std::clamp(cosI, 0.0f, 1.0f);
    const float sinT = etaI / etaT * std::sqrt(std::max(0.0f, 1.0f - cosI * cosI));
    if (sinT >= 1.0f) return 1.0f;
    const float cosT = std::sqrt(std::max(0.0f, 1.0f - sinT * sinT));
    const float rs = (etaI * cosI - etaT * cosT) / (etaI * cosI + etaT * cosT);
    const float rp = (etaT * cosI - etaI * cosT) / (etaT * cosI + etaI * cosT);
    return 0.5f * (rs * rs + rp * rp);
}

bool refractDirection(glm::vec3 i, glm::vec3 n, float eta, glm::vec3& t) {
    const float cosI = -glm::dot(n, i);
    float k = 1.0f - eta * eta * (1.0f - cosI * cosI);
    if (k < 0.0f) {
        // Only rounding may be clamped; real total internal reflection stays one.
        if (k < -1e-6f) return false;
        k = 0.0f;
    }
    t = eta * i + (eta * cosI - std::sqrt(k)) * n;
    return true;
}

glm::vec3 absorptionCoefficient(glm::vec3 color, float distance) {
    if (!(distance > 0.0f)) return glm::vec3(0.0f);
    const glm::vec3 c = glm::clamp(color, glm::vec3(1e-4f), glm::vec3(1.0f));
    return -glm::log(c) / distance;
}

glm::vec3 transmittance(glm::vec3 sigma, float pathLength) { return glm::exp(-sigma * std::max(pathLength, 0.0f)); }

void anisotropicAlphas(float roughness, float anisotropy, float& alphaT, float& alphaB) {
    const float alpha = std::max(roughness * roughness, 0.0025f);
    const float a = std::clamp(anisotropy, -1.0f, 1.0f);
    const float aspect = std::sqrt(1.0f - 0.9f * std::abs(a));
    alphaT = alpha / aspect;
    alphaB = alpha * aspect;
    if (a < 0.0f) std::swap(alphaT, alphaB);
}

float anisotropicD(glm::vec3 h, glm::vec3 t, glm::vec3 b, glm::vec3 n, float alphaT, float alphaB) {
    const float x = glm::dot(h, t) / alphaT, y = glm::dot(h, b) / alphaB, z = glm::dot(h, n);
    const float d = x * x + y * y + z * z;
    return 1.0f / (kPi * alphaT * alphaB * d * d);
}

float anisotropicLambda(glm::vec3 w, glm::vec3 t, glm::vec3 b, glm::vec3 n, float alphaT, float alphaB) {
    const float c = glm::dot(w, n);
    const float x = alphaT * glm::dot(w, t), y = alphaB * glm::dot(w, b);
    return (std::sqrt(1.0f + (x * x + y * y) / std::max(c * c, 1e-8f)) - 1.0f) * 0.5f;
}

float anisotropicSpecular(glm::vec3 l, glm::vec3 v, glm::vec3 t, glm::vec3 b, glm::vec3 n, float alphaT, float alphaB) {
    const float nDotL = glm::dot(n, l), nDotV = glm::dot(n, v);
    if (nDotL <= 0.0f || nDotV <= 0.0f) return 0.0f;
    const glm::vec3 sum = l + v;
    const float length = glm::length(sum);
    if (length < 1e-6f) return 0.0f;
    const glm::vec3 h = sum / length;
    const float g =
        1.0f / (1.0f + anisotropicLambda(l, t, b, n, alphaT, alphaB) + anisotropicLambda(v, t, b, n, alphaT, alphaB));
    return anisotropicD(h, t, b, n, alphaT, alphaB) * g / (4.0f * nDotL * nDotV);
}

float isotropicD(float nDotH, float alpha) {
    const float a2 = alpha * alpha;
    const float d = nDotH * nDotH * (a2 - 1.0f) + 1.0f;
    return a2 / (kPi * d * d);
}

float projectedRadius(float radiusMetres, float fy, float z, bool orthographic, float pixelsPerMetre) {
    if (orthographic) return radiusMetres * pixelsPerMetre;
    return radiusMetres * fy / std::max(z, 1e-4f);
}

float skinKernelWeight(float offsetPixels, float radiusPixels) {
    if (offsetPixels == 0.0f) return 1.0f;
    if (!(radiusPixels > 0.0f)) return 0.0f;
    const float sigma = radiusPixels * 0.5f;
    return std::exp(-offsetPixels * offsetPixels / (2.0f * sigma * sigma));
}

}   // namespace shockwave
