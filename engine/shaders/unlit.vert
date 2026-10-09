#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require

// Vertex pulling: no vertex input state. The vertex buffer is read straight from
// its GPU address, passed in push constants (buffer device address).
struct Vertex {
    vec3 position;
    vec3 color;
};

layout(buffer_reference, scalar) readonly buffer VertexBuffer {
    Vertex vertices[];
};

layout(push_constant, scalar) uniform PushConstants {
    mat4 mvp;
    VertexBuffer vertexBuffer;
} pc;

layout(location = 0) out vec3 vColor;

void main() {
    Vertex v = pc.vertexBuffer.vertices[gl_VertexIndex];
    gl_Position = pc.mvp * vec4(v.position, 1.0);
    vColor = v.color;
}
