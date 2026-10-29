#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_GOOGLE_include_directive : require

// GPU-driven layout: one push constant points at this frame's draw table, each draw
// finds its entry through gl_InstanceIndex (firstInstance), and the entry points at
// the mesh's vertices. No vertex input state, no per-draw descriptor binding.
#include "draw.glsl"

layout(push_constant, scalar) uniform PushConstants {
    DrawBuffer drawBuffer;
} pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec4 vUvs;   // UV0 (xy) and UV1 (zw), tiled
layout(location = 2) out vec4 vColor;
layout(location = 3) flat out uint vTexture;
layout(location = 4) flat out float vAlphaCutoff;
layout(location = 5) out vec3 vWorldPosition;
layout(location = 6) flat out vec2 vRoughnessMetallic;
layout(location = 7) flat out vec3 vEmissive;
layout(location = 8) out vec4 vTangent;   // world space; w: bitangent handedness
layout(location = 9) flat out uvec4 vMaps;   // normal, metallic-roughness, emissive, occlusion
layout(location = 10) flat out vec2 vMapScales;   // normal scale, occlusion strength
layout(location = 11) flat out uint vDraw;   // this draw's entry, for its slot bindings

void main() {
    DrawData draw = pc.drawBuffer.draws[gl_InstanceIndex];
    // Stored as an address: MoltenVK (macOS) reads references kept inside buffers as null.
    Vertex v = VertexBuffer(draw.vertexBuffer).vertices[gl_VertexIndex];

    gl_Position = draw.mvp * vec4(v.position, 1.0);
    vNormal = mat3(draw.normalMatrix) * v.normal;
    vUvs = vertexUvs(draw, v, gl_VertexIndex);
    vDraw = gl_InstanceIndex;
    vColor = v.color * draw.tint;
    vTexture = draw.textureIndex;
    vAlphaCutoff = draw.alphaCutoff;
    vWorldPosition = (draw.model * vec4(v.position, 1.0)).xyz;
    vRoughnessMetallic = vec2(draw.roughness, draw.metallic);
    vEmissive = draw.emissive;
    // A mirroring transform flips which way the bitangent points.
    mat3 linear = mat3(draw.model);
    vTangent = vec4(linear * v.tangent.xyz, determinant(linear) < 0.0 ? -v.tangent.w : v.tangent.w);
    vMaps = uvec4(draw.normalTexture, draw.metallicRoughnessTexture, draw.emissiveTexture, draw.occlusionTexture);
    vMapScales = vec2(draw.normalScale, draw.occlusionStrength);
}
