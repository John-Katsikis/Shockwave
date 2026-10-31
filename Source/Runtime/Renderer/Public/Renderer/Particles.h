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

// 52 bytes, tightly packed: renderers may copy it to the GPU as is.
struct ParticleVertex {
    glm::vec3 position;   // world space
    uint32_t color;   // RGBA8, sRGB-encoded rgb (r in the low byte)
    glm::vec2 uv;   // color texture
    glm::vec2 alphaUV;   // alpha texture (ParticleBatch::alphaTexture)
    glm::vec2 nextUV;   // the next flipbook frame, blended in by flipbookRate
    uint32_t normal;   // world normal for lit batches: xyz as unorm8 mapping -1..1
    uint32_t params;   // two halves (packHalf2x16): x flipbook rate 0..1, y alpha threshold
    // World tangent (the texture's +u direction), packed like normal: distortion batches
    // displace the scene along it and the binormal (normal x tangent).
    uint32_t tangent;
};
static_assert(sizeof(ParticleVertex) == 52, "ParticleVertex is 52 bytes");

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
// Heat haze (Effekseer's background distortion, A04): instead of its own colour, the
// batch shows the scene behind it, displaced. The texture's red and green, mapped to
// -1..1 and times the vertex colour's red and green, move the scene's look-up point by
// up to ParticleBatch::distortion metres along the tangent and binormal, as projected
// on screen. Drawn only with the particle_distortion setting on.
inline constexpr uint32_t kParticleDistortion = 1u << 5;

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
    // Soft particles (A04), in metres, Effekseer's meaning: the batch fades in over
    // softDistance behind it to the scene surface behind it (no hard seam where smoke
    // meets a wall), and over softNear past softNearOffset from the camera. 0 turns that
    // fade off. Drawn only with the soft_particles setting on.
    float softDistance = 0.0f;
    float softNear = 0.0f, softNearOffset = 0.0f;
    float distortion = 0.0f;   // kParticleDistortion: metres of displacement at full strength
    // What it was sorted by: view distance (metres in front of the camera) of the effect,
    // or of this part of it (Effects' sorting). Batches arrive back to front; renderers keep
    // their order.
    float depth = 0.0f;
};

// How lit batches (kParticleLit) are lit: the player's particle_lighting setting.
enum class ParticleLighting : uint8_t {
    Simple,   // ParticleDrawList's one directional light and ambient
    Scene,   // every light in the frame, point and spot lights through clusters (G11)
};
const char* particleLightingName(ParticleLighting lighting);   // "simple", "scene"

// The soft particle fade (particle.frag does the same), Effekseer's: 0 hides the
// fragment, 1 leaves it be. gap: metres from the particle to the scene surface behind it
// (infinite over the sky); z: the particle's distance in front of the camera. Each fade
// whose distance is 0 is off. The result multiplies alpha; every blend mode fades to
// leaving the scene as it was (see docs/Effects.md).
float softParticleFade(float gap, float z, float softDistance, float softNear, float softNearOffset);

// The player's particle settings (Renderer::setParticleSettings). Each feature can be
// turned off; off costs nothing.
struct ParticleSettings {
    bool softParticles = true;   // soft_particles
    bool distortion = true;   // particle_distortion; off skips distortion batches
    ParticleLighting lighting = ParticleLighting::Scene;   // particle_lighting
    bool operator==(const ParticleSettings&) const = default;
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

// What the renderer did with last frame's particles (Renderer::particleStats), for the
// benchmark and the Stats window. The GPU draw itself is the "particles" pass timing.
struct ParticleStats {
    float uploadMs = 0;   // CPU, main thread: gathering the draw lists and copying them to the GPU
    uint64_t uploadBytes = 0;   // vertices, indices and batch data copied to GPU memory
    uint32_t vertices = 0;
    uint32_t draws = 0;
    uint32_t softDraws = 0;   // batches faded against the scene's depth
    uint32_t distortionDraws = 0;   // distortion batches drawn
    uint32_t skippedDistortion = 0;   // distortion batches not drawn: particle_distortion off
    uint32_t sceneLitDraws = 0;   // lit batches lit by every light (particle_lighting scene)
    bool depthPass = false;   // the frame drew depth-writing batches in a pass of their own first
};

}   // namespace shockwave
