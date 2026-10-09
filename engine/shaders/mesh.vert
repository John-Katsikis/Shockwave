#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require

// GPU-driven layout: one push constant points at this frame's draw table, each draw
// finds its entry through gl_InstanceIndex (firstInstance), and the entry points at
// the mesh's vertices. No vertex input state, no per-draw descriptor binding.
struct Vertex {
    vec3 position;
    vec3 normal;
    vec2 uv;
    vec4 color;
};

layout(buffer_reference, scalar) readonly buffer VertexBuffer {
    Vertex vertices[];
};

// Must match DrawData in Renderer.cpp.
struct DrawData {
    mat4 mvp;
    mat4 normalMatrix;
    vec4 tint;
    VertexBuffer vertexBuffer;
    uint textureIndex;
    uint pad;
};

layout(buffer_reference, scalar) readonly buffer DrawBuffer {
    DrawData draws[];
};

layout(push_constant, scalar) uniform PushConstants {
    DrawBuffer drawBuffer;
} pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUv;
layout(location = 2) out vec4 vColor;
layout(location = 3) flat out uint vTexture;

void main() {
    DrawData draw = pc.drawBuffer.draws[gl_InstanceIndex];
    Vertex v = draw.vertexBuffer.vertices[gl_VertexIndex];

    gl_Position = draw.mvp * vec4(v.position, 1.0);
    vNormal = mat3(draw.normalMatrix) * v.normal;
    vUv = v.uv;
    vColor = v.color * draw.tint;
    vTexture = draw.textureIndex;
}
