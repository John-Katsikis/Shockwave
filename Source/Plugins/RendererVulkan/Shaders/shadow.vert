#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_GOOGLE_include_directive : require

// Shadow maps: the scene's draw table seen from a light. Same vertex pulling as mesh.vert.
#include "draw.glsl"

// Must match ShadowPushConstants in RendererImpl.h.
layout(push_constant, scalar) uniform PushConstants {
    DrawBuffer drawBuffer;
    uvec2 pad;
    mat4 viewProjection;
} pc;

layout(location = 0) out vec4 vUvs;   // UV0 (xy) and UV1 (zw), tiled
layout(location = 1) out float vAlpha;
layout(location = 2) flat out uint vDraw;

void main() {
    DrawData draw = pc.drawBuffer.draws[gl_InstanceIndex];
    // Stored as an address: MoltenVK (macOS) reads references kept inside buffers as null.
    Vertex v = VertexBuffer(draw.vertexBuffer).vertices[gl_VertexIndex];
    gl_Position = pc.viewProjection * (draw.model * vec4(v.position, 1.0));
    vUvs = vertexUvs(draw, v, gl_VertexIndex);
    vAlpha = v.color.a * draw.tint.a;
    vDraw = gl_InstanceIndex;
}
