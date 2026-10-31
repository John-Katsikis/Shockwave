#version 460
#extension GL_EXT_scalar_block_layout : require

// The environment map behind everything (RendererEnvironment.cpp): one triangle covering
// the scene at the far plane (reverse-Z: depth 0), drawn before anything else.
layout(push_constant, scalar) uniform PushConstants {
    mat4 inverseViewProjection;   // of the camera's rotation alone
    uvec2 frame;   // read by sky.frag (fog)
    vec4 background;   // read by sky.frag
} pc;

layout(location = 0) out vec4 vDirection;

void main() {
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vec2 ndc = corner * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    // A point on the near plane, seen from the origin: the view direction through this
    // pixel once divided by w (per pixel, after interpolation).
    vDirection = pc.inverseViewProjection * vec4(ndc, 1.0, 1.0);
}
