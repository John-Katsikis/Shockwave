#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require

// Game UI: 2D vertices pulled from this frame's UI buffer, in output pixels.
struct UiVertex {
    vec2 position;
    uint color;   // premultiplied RGBA8, sRGB
    vec2 uv;
};

layout(buffer_reference, scalar) readonly buffer UiVertices {
    UiVertex vertices[];
};

// Must match UiPushConstants in RendererImpl.h.
layout(push_constant, scalar) uniform PushConstants {
    mat4 transform;
    UiVertices vertices;
    uint texture;
    uint pad;
} pc;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outUv;

void main() {
    UiVertex v = pc.vertices.vertices[gl_VertexIndex];
    gl_Position = pc.transform * vec4(v.position, 0.0, 1.0);
    outColor = unpackUnorm4x8(v.color);
    outUv = v.uv;
}
