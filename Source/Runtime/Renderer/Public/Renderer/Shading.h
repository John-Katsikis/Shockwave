#pragma once

// Character and specialized shading (A07-A09, docs/Graphics/05-CharacterShading.md,
// docs/Shading.md): clearcoat, transmission (glass), screen-space skin diffusion and
// anisotropic hair cards. The maths here is what the shaders do (mesh.frag, skin_blur.comp):
// tests check it by hand, and the GPU tests check the shaders against it.

#include <Renderer/RenderTypes.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace shockwave {

// A material's shading family (.swmaterial "family"). Standard can add a clearcoat.
enum class ShadingFamily : uint32_t { Standard, Skin, HairCard, Transmission };
const char* shadingFamilyName(ShadingFamily family);   // "standard", "skin", "hair_card", "transmission"

// ---- Settings (GraphicsSettings: clearcoat, transmission_quality, skin_quality, hair_quality) ----
// Transmission: off draws glass as simplified see-through blended glass (never invisible
// or opaque); low refracts nothing (thin glass, roughness-blurred background); medium
// refracts through the thickness; high also blurs rough glass with four jittered taps.
enum class TransmissionQuality : uint32_t { Off, Low, Medium, High };
const char* transmissionQualityName(TransmissionQuality quality);
// Skin: off draws skin as the opaque skin BRDF without diffusion (no pass, no memory);
// low blurs with 7 taps a direction, high 13.
enum class SkinQuality : uint32_t { Off, Low, High };
const char* skinQualityName(SkinQuality quality);
// Hair: low shades cards with the isotropic highlight; high draws it as a band across the strands.
enum class HairQuality : uint32_t { Low, High };
const char* hairQualityName(HairQuality quality);
struct ShadingSettings {
    bool clearcoat = true;   // off: coated materials draw as their base (standard) material
    TransmissionQuality transmission = TransmissionQuality::Medium;
    SkinQuality skin = SkinQuality::High;
    HairQuality hair = HairQuality::High;
    bool operator==(const ShadingSettings&) const = default;
};
uint32_t skinBlurTaps(SkinQuality quality);   // per direction: 0, 7, 13

// What a material's family adds to DrawMaterial (MaterialShading lives with the material
// asset; DrawMaterial::shading points at it during the draw call).
inline constexpr size_t kShadingSlots = 3;   // MaterialSlot::Clearcoat onwards
struct MaterialShading {
    ShadingFamily family = ShadingFamily::Standard;
    // Clearcoat (A07a): a dielectric coat (IOR 1.5) over the base. 0: none.
    float clearcoat = 0.0f;
    float clearcoatRoughness = 0.0f;   // perceptual
    float clearcoatNormalScale = 1.0f;
    TextureHandle clearcoatTexture;   // linear: red scales clearcoat, green clearcoatRoughness
    TextureHandle clearcoatNormalTexture;   // 0: the coat follows the geometric normal
    // Transmission (A07b): light through a dielectric.
    float transmission = 0.0f;   // 0..1 share of the diffuse that's transmitted
    float ior = 1.5f;
    float thickness = 0.01f;   // metres, in the object's space (its scale applies)
    bool thin = true;   // thin: a sheet, light leaves where it entered; solid: refracts through
    glm::vec3 attenuationColor{1.0f};   // what's left after attenuationDistance
    float attenuationDistance = 0.0f;   // metres; 0: no absorption
    TextureHandle transmissionTexture;   // linear: red scales transmission, green thickness
    // Skin (A08): screen-space diffusion of the diffuse light.
    glm::vec3 skinRadius{3.7f, 1.4f, 1.1f};   // millimetres, per channel
    float skinStrength = 1.0f;   // 0..1
    float skinMaxPixels = 24.0f;   // the blur's screen radius never passes this
    // Hair cards (A09): anisotropic GGX along the strands.
    float hairAnisotropy = 0.8f;   // -1..1; 0 is isotropic, positive a band across the strands
    uint32_t hairDirection = 1;   // root to tip: 0 along +u, 1 along +v (down the texture)
    std::array<TextureBinding, kShadingSlots> bindings{};
};

// ---- Clearcoat ----
inline constexpr float kCoatF0 = 0.04f;   // ((1.5 - 1) / (1.5 + 1))^2
float schlickFresnel(float f0, float cosTheta);
// How much of the base's light gets through the coat both ways:
// (1 - c F(n.l)) (1 - c F(n.v)), F the coat's Schlick Fresnel.
float coatBaseWeight(float coat, float nDotL, float nDotV);

// ---- Transmission ----
// Exact unpolarized dielectric Fresnel reflectance for a ray meeting the surface at cosI
// (from the side of index etaI into etaT); 1 under total internal reflection.
float dielectricFresnel(float cosI, float etaI, float etaT);
// Snell: incident propagation i (pointing into the surface, unit), outward normal n (unit),
// eta = etaIncident / etaTransmitted. False under total internal reflection.
bool refractDirection(glm::vec3 i, glm::vec3 n, float eta, glm::vec3& t);
// Absorption: sigma = -ln(color) / distance per channel (colour clamped to 1e-4..1);
// distance 0 absorbs nothing.
glm::vec3 absorptionCoefficient(glm::vec3 color, float distance);
glm::vec3 transmittance(glm::vec3 sigma, float pathLength);

// ---- Anisotropic GGX (hair cards) ----
// alpha = max(r^2, 0.0025); aspect = sqrt(1 - 0.9 |a|); alphaT = alpha / aspect,
// alphaB = alpha * aspect; swapped for a < 0.
// t runs along the strands; a hair_card's hair_anisotropy arrives negated (positive there
// is the band across the strands).
void anisotropicAlphas(float roughness, float anisotropy, float& alphaT, float& alphaB);
float anisotropicD(glm::vec3 h, glm::vec3 t, glm::vec3 b, glm::vec3 n, float alphaT, float alphaB);
float anisotropicLambda(glm::vec3 w, glm::vec3 t, glm::vec3 b, glm::vec3 n, float alphaT, float alphaB);
// D G / (4 n.l n.v), without Fresnel; 0 unless both cosines are positive.
float anisotropicSpecular(glm::vec3 l, glm::vec3 v, glm::vec3 t, glm::vec3 b, glm::vec3 n, float alphaT, float alphaB);
float isotropicD(float nDotH, float alpha);

// ---- Skin diffusion ----
// A world radius on screen: radius * fy / z for perspective (fy: vertical focal length in
// pixels), radius * pixelsPerMetre for orthographic (z ignored).
float projectedRadius(float radiusMetres, float fy, float z, bool orthographic, float pixelsPerMetre = 0.0f);
// The blur's weight for a tap offsetPixels from the centre, for a channel whose diffusion
// radius is radiusPixels: a Gaussian with sigma radius / 2. The centre is always 1.
float skinKernelWeight(float offsetPixels, float radiusPixels);

// The last frame's (Renderer::shadingStats).
struct ShadingStats {
    uint32_t coatDraws = 0;   // drawn with their coat
    uint32_t transmissionDraws = 0;   // through the transmission pass
    uint32_t simplifiedGlassDraws = 0;   // transmission off: blended stand-in
    uint32_t skinDraws = 0;   // in the skin pass
    uint32_t hairDraws = 0;
    bool anisotropicHair = false;
    bool transmissionPass = false, skinPass = false;
    uint64_t transmissionBytes = 0, skinBytes = 0;
};

}   // namespace shockwave
