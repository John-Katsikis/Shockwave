#pragma once

// Particle geometry for one frame, in a form any renderer plugin can draw: world-space
// vertices, triangle indices and batches that share a texture and blend state. The
// Effects module builds it from Effekseer's simulation (docs/Effects.md); renderers know
// nothing about Effekseer. Renderer::drawParticles hands it to the backend, which draws
// it into the HDR scene, depth tested against it, after the opaque geometry.

#include <Renderer/RenderTypes.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <span>

namespace shockwave {

// 48 bytes, tightly packed: renderers may copy it to the GPU as is.
struct ParticleVertex {
    glm::vec3 position;   // world space
    uint32_t color;   // RGBA8, sRGB-encoded rgb (r in the low byte)
    glm::vec2 uv;   // color texture
    glm::vec2 alphaUV;   // alpha texture (ParticleBatch::alphaTexture)
    glm::vec2 nextUV;   // the next flipbook frame, blended in by flipbookRate
    uint32_t normal;   // world normal for lit batches: xyz as unorm8 mapping -1..1
    uint32_t params;   // two halves (packHalf2x16): x flipbook rate 0..1, y alpha threshold
};
static_assert(sizeof(ParticleVertex) == 48, "ParticleVertex is 48 bytes");

enum class ParticleBlend : uint8_t {
    Opaque,   // replaces what's behind
    Alpha,   // the usual "over" blend
    Add,   // brightens: fire, sparks, glows
    Subtract,   // darkens by the color
    Multiply,   // tints what's behind
};

enum class ParticleCull : uint8_t { None, Front, Back };

// Flags in ParticleBatch::flags.
inline constexpr uint32_t kParticleLit = 1u << 0;   // shaded by the light in ParticleDrawList
inline constexpr uint32_t kParticleFlipbookBlend = 1u << 1;   // blend uv and nextUV by the flipbook rate
inline constexpr uint32_t kParticleAlphaTexture = 1u << 2;   // alpha multiplied by alphaTexture's red * alpha
inline constexpr uint32_t kParticleClampUV = 1u << 3;   // clamp uv instead of repeating
inline constexpr uint32_t kParticleAlphaCutoff = 1u << 4;   // discard at or below the vertex alpha threshold

struct ParticleBatch {
    uint32_t firstIndex = 0, indexCount = 0;   // a range of ParticleDrawList::indices
    TextureHandle texture;   // color; 0 is white
    TextureHandle alphaTexture;
    ParticleBlend blend = ParticleBlend::Alpha;
    ParticleCull cull = ParticleCull::None;
    bool depthTest = true;   // against the scene (reverse-Z)
    bool depthWrite = false;
    uint32_t flags = 0;
    glm::vec4 tint{1.0f};   // linear, multiplies the color; rgb may exceed 1 for HDR glow
    float emissive = 1.0f;   // multiplies rgb
    glm::vec4 edgeColor{0.0f};   // alpha cutoff edge: rgb (linear, scaled), a = edge width
    // Sort key, like a mesh's: the reverse-Z depth of the effect's origin (greater is
    // nearer). Batches arrive back to front; renderers keep their order.
    float depth = 0.0f;
};

struct ParticleDrawList {
    std::span<const ParticleVertex> vertices;
    std::span<const uint32_t> indices;   // into vertices, three per triangle
    std::span<const ParticleBatch> batches;
    // For lit batches: one directional light and an ambient term, both linear.
    glm::vec3 lightDirection{0.0f, -1.0f, 0.0f};   // where the light shines
    glm::vec3 lightColor{1.0f};
    glm::vec3 ambient{0.2f};
};

}   // namespace shockwave
