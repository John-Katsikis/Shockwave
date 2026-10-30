#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_GOOGLE_include_directive : require

// The decal pass (G08, RendererDecals.cpp): each decal is drawn as its box, back faces
// only and without a depth test, so every pixel whose visible surface might lie inside
// is shaded exactly once, the camera inside the box included. decal.frag decides.
#include "frame.glsl"
#include "decal.glsl"

layout(push_constant, scalar) uniform PushConstants {
    FrameBuffer frame;
    DecalBuffer decals;
} pc;

layout(location = 0) flat out uint vDecal;

// The unit cube's 12 triangles, counterclockwise seen from outside.
const vec3 kCorners[8] = vec3[](
    vec3(-0.5, -0.5, -0.5), vec3(0.5, -0.5, -0.5), vec3(0.5, 0.5, -0.5), vec3(-0.5, 0.5, -0.5),
    vec3(-0.5, -0.5, 0.5), vec3(0.5, -0.5, 0.5), vec3(0.5, 0.5, 0.5), vec3(-0.5, 0.5, 0.5));
const uint kIndices[36] = uint[](
    0, 2, 1, 0, 3, 2,   // -z
    4, 5, 6, 4, 6, 7,   // +z
    0, 1, 5, 0, 5, 4,   // -y
    3, 7, 6, 3, 6, 2,   // +y
    0, 4, 7, 0, 7, 3,   // -x
    1, 2, 6, 1, 6, 5);  // +x

void main() {
    DecalData d = pc.decals.decals[gl_InstanceIndex];
    vec4 world = d.boxToWorld * vec4(kCorners[kIndices[gl_VertexIndex]], 1.0);
    gl_Position = pc.frame.projection * pc.frame.view * world;
    vDecal = gl_InstanceIndex;
}
