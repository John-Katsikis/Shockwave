#pragma once

// Projected decals (G08, docs/GraphicsImplementation.md 12; docs/Decals.md): the contract
// between the engine, which hands the renderer this frame's decals, and the renderer, which
// projects them into decal buffers after the surface prepass and has opaque surfaces read
// those buffers before lighting. Shaders/decal.frag and mesh.frag are the same code in GLSL;
// the tests check the rules written here.
//
// A decal is a box: the unit cube [-0.5, 0.5]^3 taken to the world by boxToWorld (its size
// and its entity's transform). It projects along its local -Z onto whatever visible opaque
// surface lies inside the box, facing back along +Z. Texture u runs along local +X and v
// down local -Y, so an unrotated decal reads upright on a wall facing +Z.
//
// Decals are composed in order (sortOrder, then order) with premultiplied "over":
//   C' = c a + C (1 - a),   A' = a + A (1 - a)
// starting from C = 0, A = 0 at every pixel, and applied once to the surface:
//   albedo = base (1 - A) + C
// in linear colour. Roughness and metallic do the same in their own buffer.

#include <Renderer/RenderTypes.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace shockwave {

// The decals_quality setting: how many decals a frame draws at most, nearest first.
// Off draws none and has no decal pass, no buffers and no prepass for them.
enum class DecalQuality : uint32_t { Off, Low, Medium, High };
const char* decalQualityName(DecalQuality quality);   // "off", "low", "medium", "high"
uint32_t decalBudget(DecalQuality quality);   // 0, 64, 256, 1024
// The most decals a frame can submit; more are dropped (and counted) before the budget.
inline constexpr uint32_t kMaxSubmittedDecals = 8192;

// What a decal changes.
inline constexpr uint32_t kDecalAlbedo = 1u;   // base colour
inline constexpr uint32_t kDecalRoughnessMetallic = 2u;   // roughness and metallic, together
// Receiver layers: a surface is on layer bits (DrawMaterial::decalLayers, 1 by default) and
// a decal lands only where receiverMask shares a bit with them. Eight layers.
inline constexpr uint32_t kDecalLayerMask = 0xFFu;

struct DecalDesc {
    glm::mat4 boxToWorld{1.0f};   // the unit box to the world: size, rotation, position
    TextureHandle texture;   // colour (sRGB textures decode to linear) and alpha; none: white
    glm::vec4 color{1.0f};   // linear rgb times the texture; a is the opacity, 0..1
    float roughness = 0.5f, metallic = 0.0f;   // with kDecalRoughnessMetallic
    uint32_t channels = kDecalAlbedo;
    uint32_t receiverMask = kDecalLayerMask;
    // Surfaces turned further than this from facing the decal fade, reaching nothing at 90
    // degrees (side-on), so a decal doesn't smear down the sides of what it hits.
    float angleFadeDegrees = 60.0f;
    int32_t sortOrder = 0;   // higher draws over lower
    uint64_t order = 0;   // a stable id: equal sort orders draw lower first
    bool essential = false;   // a gameplay marker: kept first when the budget is short
};

// What the last frame did with its decals.
struct DecalStats {
    uint32_t submitted = 0;   // handed to the renderer
    uint32_t visible = 0;   // of those, on screen
    uint32_t drawn = 0;   // of those, within the budget and drawn
    uint32_t overBudget = 0;   // visible but left out by the budget
};

// ---- Validation ----

// Each size component finite and above zero, and the matrix invertible and finite.
bool validDecal(const DecalDesc& decal);

// ---- Per pixel ----

// The point in the decal's unit box; false when it's outside (or the box is degenerate).
inline bool decalLocal(const DecalDesc& decal, const glm::vec3& world, glm::vec3& local) {
    local = glm::vec3(glm::inverse(decal.boxToWorld) * glm::vec4(world, 1.0f));
    return std::isfinite(local.x) && std::isfinite(local.y) && std::isfinite(local.z) &&
           glm::all(glm::lessThanEqual(glm::abs(local), glm::vec3(0.5f)));
}
inline glm::vec2 decalUv(const glm::vec3& local) { return {local.x + 0.5f, 0.5f - local.y}; }
// The world direction surfaces must face to take the decal in full: the box's +Z.
inline glm::vec3 decalFacing(const DecalDesc& decal) {
    const glm::vec3 z(decal.boxToWorld[2]);
    const float length = glm::length(z);
    return length > 0.0f ? z / length : glm::vec3(0.0f, 0.0f, 1.0f);
}
// 1 for a surface facing the decal within angleFadeDegrees, falling linearly in cosine to
// 0 side-on, 0 for surfaces facing away. normal is unit length, in the world.
inline float decalAngleFade(const DecalDesc& decal, const glm::vec3& normal) {
    const float c = glm::dot(normal, decalFacing(decal));
    if (!(c > 0.0f)) return 0.0f;
    const float start = std::cos(glm::radians(std::clamp(decal.angleFadeDegrees, 0.0f, 89.0f)));
    return std::clamp(c / start, 0.0f, 1.0f);
}

// The premultiplied "over" above, one decal at a time, and its single application.
struct DecalAccumulation {
    glm::vec3 color{0.0f};   // premultiplied
    float alpha = 0.0f;
};
inline void composeDecal(DecalAccumulation& into, const glm::vec3& color, float alpha) {
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    into.color = color * alpha + into.color * (1.0f - alpha);
    into.alpha = alpha + into.alpha * (1.0f - alpha);
}
inline glm::vec3 applyDecals(const glm::vec3& base, const DecalAccumulation& decals) {
    return base * (1.0f - decals.alpha) + decals.color;
}

// ---- Which decals a frame draws ----

// Whether any of the box can be on screen: false only when all eight corners are outside
// one clip plane. viewProjection is reverse-Z (0 <= z <= w in front).
bool decalMayBeVisible(const DecalDesc& decal, const glm::mat4& viewProjection);
// Up to budget of the decals given, by index, in drawing order. When there are more than
// budget, essential ones are kept first, then the nearest to the camera (from the box's
// centre), then the lower order, so the same scene always keeps the same ones. The kept
// ones are then sorted by sortOrder, then order.
std::vector<uint32_t> selectDecals(std::span<const DecalDesc> decals, const glm::vec3& camera, uint32_t budget);

}   // namespace shockwave
