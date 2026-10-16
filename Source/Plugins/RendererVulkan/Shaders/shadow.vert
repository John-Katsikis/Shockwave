#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_scalar_block_layout : require

// Shadow maps: the scene's draw table seen from a light. Same vertex pulling as mesh.vert.
struct Vertex {
    vec3 position;
    vec3 normal;
    vec2 uv;
    vec4 color;
    vec4 tangent;   // xyz along +u; w: handedness of the bitangent
};

layout(buffer_reference, scalar) readonly buffer VertexBuffer {
    Vertex vertices[];
};

// Must match DrawData in RendererImpl.h.
struct DrawData {
    mat4 mvp;
    mat4 model;
    mat4 normalMatrix;
    vec4 tint;
    vec2 uvScale;
    uvec2 vertexBuffer;   // a VertexBuffer; see below
    uint textureIndex;
    float alphaCutoff;
    float roughness;
    float metallic;
    vec3 emissive;
    uint normalTexture;   // 0: none
    uint metallicRoughnessTexture;
    uint emissiveTexture;
    uint occlusionTexture;
    float normalScale;
    float occlusionStrength;
    uint pad;
};

layout(buffer_reference, scalar) readonly buffer DrawBuffer {
    DrawData draws[];
};

// Must match ShadowPushConstants in RendererImpl.h.
layout(push_constant, scalar) uniform PushConstants {
    DrawBuffer drawBuffer;
    uvec2 pad;
    mat4 viewProjection;
} pc;

layout(location = 0) out vec2 vUv;
layout(location = 1) out float vAlpha;
layout(location = 2) flat out uint vTexture;
layout(location = 3) flat out float vAlphaCutoff;

void main() {
    DrawData draw = pc.drawBuffer.draws[gl_InstanceIndex];
    // Stored as an address: MoltenVK (macOS) reads references kept inside buffers as null.
    Vertex v = VertexBuffer(draw.vertexBuffer).vertices[gl_VertexIndex];
    gl_Position = pc.viewProjection * (draw.model * vec4(v.position, 1.0));
    vUv = v.uv * draw.uvScale;
    vAlpha = v.color.a * draw.tint.a;
    vTexture = draw.textureIndex;
    vAlphaCutoff = draw.alphaCutoff;
}
