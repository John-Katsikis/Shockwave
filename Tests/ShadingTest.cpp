// Character and specialized shading contracts (A07-A09; docs/Graphics/05-CharacterShading.md):
// clearcoat energy, Snell and Fresnel, absorption, anisotropic GGX and the skin kernel,
// checked against hand-worked values and numeric integrals. No GPU.
#include <Renderer/Shading.h>

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
bool near(float a, float b, float tolerance = 1e-5f) { return std::abs(a - b) <= tolerance; }
constexpr float kPi = 3.14159265358979f;

glm::vec3 sphericalDirection(float theta, float phi) {
    return {std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta)};
}

void testClearcoat() {
    // No coat lets everything through: the base is unchanged (coat 0 is the standard material).
    for (float c : {0.0f, 0.3f, 0.7f, 1.0f}) check(near(coatBaseWeight(0.0f, c, 1.0f - c), 1.0f), "coat 0 is identity");
    check(near(schlickFresnel(kCoatF0, 1.0f), 0.04f), "coat F0 at normal incidence");
    check(near(schlickFresnel(kCoatF0, 0.0f), 1.0f), "coat Fresnel at grazing is 1");
    // Full coat at normal incidence: 0.96^2 of the base gets through both ways.
    check(near(coatBaseWeight(1.0f, 1.0f, 1.0f), 0.96f * 0.96f), "full coat at normal incidence");
    // White furnace: coat reflectance plus what reaches the base (a white Lambertian) never
    // passes one, for any view angle.
    for (float nDotV : {1.0f, 0.7f, 0.3f, 0.05f}) {
        float coatLight = schlickFresnel(kCoatF0, nDotV);   // a mirror-like coat reflects F(v)
        float base = 0.0f;
        const int steps = 256;
        for (int i = 0; i < steps; ++i) {
            const float theta = (i + 0.5f) / steps * kPi * 0.5f;
            const float nDotL = std::cos(theta);
            // Lambertian base: integral of (1/pi) cos sin dtheta dphi = 1 without the coat.
            base += coatBaseWeight(1.0f, nDotL, nDotV) * nDotL * std::sin(theta) * 2.0f * (kPi * 0.5f / steps);
        }
        check(coatLight + base <= 1.0f + 1e-3f, "white furnace with a coat at n.v " + std::to_string(nDotV));
        check(coatLight + base > 0.85f, "coat doesn't lose much energy at n.v " + std::to_string(nDotV));
    }
}

void testTransmission() {
    check(near(dielectricFresnel(1.0f, 1.0f, 1.5f), 0.04f), "Fresnel at normal incidence (IOR 1.5)");
    check(near(dielectricFresnel(0.0f, 1.0f, 1.5f), 1.0f, 1e-4f), "Fresnel at grazing is 1");
    check(near(dielectricFresnel(1.0f, 1.5f, 1.0f), 0.04f), "Fresnel is symmetric at normal incidence");
    check(dielectricFresnel(0.3f, 1.5f, 1.0f) == 1.0f, "total internal reflection reflects everything");
    float last = 0.0f;
    bool rising = true;
    for (int i = 0; i <= 20; ++i) {
        const float f = dielectricFresnel(1.0f - i / 20.0f, 1.0f, 1.5f);
        if (f + 1e-6f < last) rising = false;
        last = f;
    }
    check(rising, "Fresnel rises towards grazing");

    const glm::vec3 n(0, 0, 1);
    glm::vec3 t;
    check(refractDirection(glm::vec3(0, 0, -1), n, 1.0f / 1.5f, t) && near(t.z, -1.0f), "straight through");
    const glm::vec3 i = glm::normalize(glm::vec3(1, 0, -1));
    check(refractDirection(i, n, 1.0f / 1.5f, t), "refracts into glass");
    check(near(glm::length(t), 1.0f, 1e-5f), "refracted direction is unit");
    // Snell: sin(theta_t) = sin(theta_i) / 1.5.
    check(near(t.x, std::sin(kPi / 4.0f) / 1.5f, 1e-5f), "Snell's law");
    check(!refractDirection(i, n, 1.5f, t), "total internal reflection out of glass at 45 degrees");
    check(refractDirection(glm::vec3(0, 0, -1), n, 1.0f, t) && near(t.z, -1.0f), "IOR 1 doesn't bend");

    // Absorption: after attenuationDistance exactly the attenuation colour is left.
    const glm::vec3 color(0.8f, 0.5f, 0.2f);
    const glm::vec3 sigma = absorptionCoefficient(color, 0.1f);
    const glm::vec3 left = transmittance(sigma, 0.1f);
    check(near(left.x, 0.8f) && near(left.y, 0.5f) && near(left.z, 0.2f), "the colour is left after the distance");
    check(transmittance(sigma, 0.0f) == glm::vec3(1.0f), "no path absorbs nothing");
    check(absorptionCoefficient(color, 0.0f) == glm::vec3(0.0f), "distance 0 absorbs nothing");
    check(absorptionCoefficient(glm::vec3(1.0f), 1.0f) == glm::vec3(0.0f), "white absorbs nothing");
    const glm::vec3 black = transmittance(absorptionCoefficient(glm::vec3(0.0f), 1.0f), 1.0f);
    check(black.x > 0.0f && black.x <= 1.1e-4f, "black is clamped, not infinite");
}

void testAnisotropy() {
    const glm::vec3 t(1, 0, 0), b(0, 1, 0), n(0, 0, 1);
    float aT = 0, aB = 0;
    anisotropicAlphas(0.5f, 0.0f, aT, aB);
    check(near(aT, 0.25f) && near(aB, 0.25f), "anisotropy 0 is isotropic");
    for (float theta : {0.0f, 0.2f, 0.6f, 1.1f}) {
        const glm::vec3 h = sphericalDirection(theta, 0.7f);
        check(
            near(anisotropicD(h, t, b, n, aT, aB), isotropicD(std::cos(theta), aT), 1e-3f),
            "anisotropic D equals isotropic D at a = 0");
    }
    anisotropicAlphas(0.5f, 0.8f, aT, aB);
    check(aT > aB, "positive anisotropy stretches along the tangent");
    check(near(aT * aB, 0.25f * 0.25f, 1e-6f), "the area of the lobe is kept");
    float sT = 0, sB = 0;
    anisotropicAlphas(0.5f, -0.8f, sT, sB);
    check(near(sT, aB) && near(sB, aT), "negative anisotropy swaps");
    anisotropicAlphas(0.0f, 0.0f, sT, sB);
    check(sT > 0.0f && sB > 0.0f, "roughness 0 is clamped");

    // Normalization: the integral of D(h) (n.h) over the hemisphere is one.
    for (float anisotropy : {0.0f, 0.5f, 0.9f}) {
        anisotropicAlphas(0.6f, anisotropy, aT, aB);
        double sum = 0.0;
        const int thetaSteps = 512, phiSteps = 256;
        for (int i = 0; i < thetaSteps; ++i) {
            const float theta = (i + 0.5f) / thetaSteps * kPi * 0.5f;
            for (int j = 0; j < phiSteps; ++j) {
                const float phi = (j + 0.5f) / phiSteps * kPi * 2.0f;
                const glm::vec3 h = sphericalDirection(theta, phi);
                sum += anisotropicD(h, t, b, n, aT, aB) * std::cos(theta) * std::sin(theta) *
                       (kPi * 0.5f / thetaSteps) * (kPi * 2.0f / phiSteps);
            }
        }
        check(std::abs(sum - 1.0) < 1e-2, "anisotropic D is normalized (a " + std::to_string(anisotropy) + ")");
    }
    // The highlight stretches along the strands: looking off-specular along t keeps more
    // light than the same angle along b.
    anisotropicAlphas(0.4f, 0.8f, aT, aB);
    const glm::vec3 l = glm::normalize(glm::vec3(0, 0, 1));
    const float alongT = anisotropicSpecular(l, glm::normalize(glm::vec3(0.4f, 0, 1)), t, b, n, aT, aB);
    const float alongB = anisotropicSpecular(l, glm::normalize(glm::vec3(0, 0.4f, 1)), t, b, n, aT, aB);
    check(alongT > alongB * 2.0f, "the highlight stretches along the tangent");
    check(anisotropicSpecular(glm::vec3(0, 0, -1), l, t, b, n, aT, aB) == 0.0f, "light from behind adds nothing");
    // Energy: the directional albedo (without Fresnel) is at most one.
    for (float viewTheta : {0.1f, 0.8f, 1.3f}) {
        const glm::vec3 v = sphericalDirection(viewTheta, 0.3f);
        double albedo = 0.0;
        const int steps = 256;
        for (int i = 0; i < steps; ++i) {
            const float theta = (i + 0.5f) / steps * kPi * 0.5f;
            for (int j = 0; j < steps; ++j) {
                const float phi = (j + 0.5f) / steps * kPi * 2.0f;
                const glm::vec3 w = sphericalDirection(theta, phi);
                albedo += anisotropicSpecular(w, v, t, b, n, aT, aB) * w.z * std::sin(theta) * (kPi * 0.5f / steps) *
                          (kPi * 2.0f / steps);
            }
        }
        check(albedo <= 1.0 + 1e-2, "anisotropic albedo at most one (view " + std::to_string(viewTheta) + ")");
    }
}

void testSkin() {
    check(
        skinBlurTaps(SkinQuality::Off) == 0 && skinBlurTaps(SkinQuality::Low) == 7 &&
            skinBlurTaps(SkinQuality::High) == 13,
        "skin blur taps");
    check(skinKernelWeight(0.0f, 0.0f) == 1.0f && skinKernelWeight(0.0f, 5.0f) == 1.0f, "kernel centre is 1");
    check(skinKernelWeight(1.0f, 0.0f) == 0.0f, "radius 0 doesn't spread");
    // sigma = r / 2, so one radius out is exp(-2).
    check(near(skinKernelWeight(4.0f, 4.0f), std::exp(-2.0f)), "one radius out is exp(-2)");
    check(skinKernelWeight(2.0f, 8.0f) > skinKernelWeight(2.0f, 2.0f), "red spreads further than blue");
    // 3.7 mm at 1 m with a 1000 px focal length: 3.7 px; twice as far is half.
    check(near(projectedRadius(0.0037f, 1000.0f, 1.0f, false), 3.7f, 1e-4f), "projected radius");
    check(near(projectedRadius(0.0037f, 1000.0f, 2.0f, false), 1.85f, 1e-4f), "further is smaller");
    check(near(projectedRadius(0.0037f, 1000.0f, 50.0f, true, 500.0f), 1.85f, 1e-4f), "orthographic ignores z");
    check(std::isfinite(projectedRadius(0.0037f, 1000.0f, 0.0f, false)), "z 0 stays finite");
}

void testNames() {
    check(std::string(shadingFamilyName(ShadingFamily::HairCard)) == "hair_card", "family names");
    check(std::string(transmissionQualityName(TransmissionQuality::Medium)) == "medium", "transmission names");
    check(std::string(skinQualityName(SkinQuality::Off)) == "off", "skin names");
    check(std::string(hairQualityName(HairQuality::High)) == "high", "hair names");
    const ShadingSettings defaults;
    check(
        defaults.clearcoat && defaults.transmission == TransmissionQuality::Medium &&
            defaults.skin == SkinQuality::High && defaults.hair == HairQuality::High,
        "default settings");
}
}   // namespace

int main() {
    testClearcoat();
    testTransmission();
    testAnisotropy();
    testSkin();
    testNames();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("shading: all checks passed\n");
    return 0;
}
