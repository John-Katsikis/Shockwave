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
layout(location = 12) out float vLayerMask;   // a second layer's mask from the vertex colours (A02)
layout(location = 13) out vec2 vLightmapUv;   // in the draw's lightmap atlas (G05); 0 without one
// Where this vertex was last frame, in last frame's unjittered clip space (G09); w 0: no
// previous pose, so the prepass writes no motion.
layout(location = 14) out vec4 vPreviousClip;
// The surface prepass (G03) and the shading over it run this same shader in different
// pipelines; their depths must match exactly for the EQUAL test.
invariant gl_Position;

void main() {
    DrawData draw = pc.drawBuffer.draws[gl_InstanceIndex];
    // Stored as an address: MoltenVK (macOS) reads references kept inside buffers as null.
    Vertex v = VertexBuffer(draw.vertexBuffer).vertices[gl_VertexIndex];

    gl_Position = draw.mvp * vec4(v.position, 1.0);
    vPreviousClip = vec4(0.0);
    if (draw.history != 0u) {
        vec3 previous = v.position;
        if (draw.previousVertexBuffer != uvec2(0u))
            previous = VertexBuffer(draw.previousVertexBuffer).vertices[gl_VertexIndex].position;
        vPreviousClip = draw.previousMvp * vec4(previous, 1.0);
    }
    vNormal = mat3(draw.normalMatrix) * v.normal;
    vUvs = vertexUvs(draw, v, gl_VertexIndex);
    vDraw = gl_InstanceIndex;
    vColor = v.color * draw.tint;
    vLayerMask = 0.0;
    if (draw.layers != uvec2(0u)) {
        // A second layer masked by a vertex colour channel: the colours mask, they don't tint.
        LayerBuffer layers = LayerBuffer(draw.layers);
        uint source = layers.layer.layerMaskSource;
        if ((layers.layer.flags & kLayerSecond) != 0u && source >= 4u) {
            vLayerMask = v.color[min(source - 4u, 2u)];
            vColor = vec4(1.0, 1.0, 1.0, v.color.a) * draw.tint;
        }
    }
    vLightmapUv = vec2(0.0);
    if (draw.indirectSource == kIndirectLightmap) vLightmapUv = LightmapUvBuffer(draw.lightmapUvBuffer).lightmapUv[gl_VertexIndex];
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
