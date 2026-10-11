#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require

// Particles: texture times vertex color times the batch's tint, in linear HDR. Blending
// (alpha, add, subtract, multiply) is fixed-function, one pipeline per mode.
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler linearSampler;

// Must match ParticleBatchData in RendererParticles.cpp.
struct ParticleBatchData {
    vec4 tint;          // linear; rgb may exceed 1
    vec4 edgeColor;     // alpha cutoff edge: rgb, a = width
    vec3 lightDirection;
    uint texture;
    vec3 lightColor;
    uint alphaTexture;
    vec3 ambient;
    uint flags;
    float emissive;
    uint blend;         // ParticleBlend
    float pad0, pad1;
};

layout(buffer_reference, scalar) readonly buffer ParticleVertices {
    uint unused[];
};
layout(buffer_reference, scalar) readonly buffer ParticleBatches {
    ParticleBatchData batches[];
};

layout(push_constant, scalar) uniform PushConstants {
    mat4 viewProjection;
    ParticleVertices vertices;
    ParticleBatches batches;
} pc;

// Flags, as in Renderer/Particles.h.
const uint kLit = 1u << 0;
const uint kFlipbookBlend = 1u << 1;
const uint kAlphaTexture = 1u << 2;
const uint kClampUv = 1u << 3;
const uint kAlphaCutoff = 1u << 4;
const uint kBlendMultiply = 4u;

layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec2 inAlphaUv;
layout(location = 3) in vec2 inNextUv;
layout(location = 4) in vec3 inNormal;
layout(location = 5) in vec2 inParams;
layout(location = 6) flat in uint inBatch;
layout(location = 0) out vec4 outColor;

vec4 sampleTexture(uint index, vec2 uv, bool clampUv) {
    if (clampUv) uv = clamp(uv, 0.0, 1.0);
    return texture(sampler2D(textures[nonuniformEXT(index)], linearSampler), uv);
}

void main() {
    ParticleBatchData b = pc.batches.batches[inBatch];
    bool clampUv = (b.flags & kClampUv) != 0u;
    vec4 tex = sampleTexture(b.texture, inUv, clampUv);
    if ((b.flags & kFlipbookBlend) != 0u) tex = mix(tex, sampleTexture(b.texture, inNextUv, clampUv), inParams.x);
    if ((b.flags & kAlphaTexture) != 0u) {
        vec4 a = sampleTexture(b.alphaTexture, inAlphaUv, clampUv);
        tex.a *= a.r * a.a;
    }
    vec4 color = tex * inColor * b.tint;
    color.rgb *= b.emissive;

    if ((b.flags & kAlphaCutoff) != 0u) {
        float threshold = inParams.y;
        if (color.a <= max(threshold, 0.0)) discard;
        // Pixels just above the threshold take the edge color: burning, dissolving edges.
        if (color.a - threshold < b.edgeColor.a) color.rgb = b.edgeColor.rgb;
    }
    if ((b.flags & kLit) != 0u) {
        vec3 n = normalize(inNormal);
        if (!gl_FrontFacing) n = -n;
        color.rgb *= max(dot(n, -b.lightDirection), 0.0) * b.lightColor + b.ambient;
    }
    // Multiply blends dst * src; fade toward white (no change) as alpha drops.
    if (b.blend == kBlendMultiply) color.rgb = mix(vec3(1.0), color.rgb, color.a);
    outColor = color;
}
