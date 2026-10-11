#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require

// Particles (Renderer/Particles.h): world-space vertices built on the CPU each frame,
// pulled from this frame's particle buffer. One draw per batch; the batch's index in
// the batch table comes in as the instance index.

// Must match ParticleVertex in Renderer/Particles.h.
struct ParticleVertex {
    vec3 position;
    uint color;      // RGBA8, sRGB-encoded rgb
    vec2 uv;
    vec2 alphaUv;
    vec2 nextUv;
    uint normal;     // xyz as unorm8, mapping -1..1
    uint params;     // half2: flipbook rate, alpha threshold
};

layout(buffer_reference, scalar) readonly buffer ParticleVertices {
    ParticleVertex vertices[];
};
layout(buffer_reference, scalar) readonly buffer ParticleBatches {
    uint unused[];
};

// Must match ParticlePushConstants in RendererParticles.cpp.
layout(push_constant, scalar) uniform PushConstants {
    mat4 viewProjection;
    ParticleVertices vertices;
    ParticleBatches batches;
} pc;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outUv;
layout(location = 2) out vec2 outAlphaUv;
layout(location = 3) out vec2 outNextUv;
layout(location = 4) out vec3 outNormal;
layout(location = 5) out vec2 outParams;
layout(location = 6) flat out uint outBatch;

vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main() {
    ParticleVertex v = pc.vertices.vertices[gl_VertexIndex];
    gl_Position = pc.viewProjection * vec4(v.position, 1.0);
    vec4 color = unpackUnorm4x8(v.color);
    outColor = vec4(srgbToLinear(color.rgb), color.a);
    outUv = v.uv;
    outAlphaUv = v.alphaUv;
    outNextUv = v.nextUv;
    outNormal = unpackUnorm4x8(v.normal).xyz * 2.0 - 1.0;
    outParams = unpackHalf2x16(v.params);
    outBatch = gl_InstanceIndex;
}
