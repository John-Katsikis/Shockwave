#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_GOOGLE_include_directive : require

// The reactive mask (G09): how much a blended surface covers each pixel, for TAA to trust
// history less where something transparent changed it. Drawn with mesh.vert over the
// scene's depth, without writing it, into one channel blended by MAX.
#define DRAW_SAMPLING
#include "draw.glsl"

layout(push_constant, scalar) uniform PushConstants {
    DrawBuffer drawBuffer;
    uvec2 frame;
} pc;

layout(location = 1) in vec4 vUvs;
layout(location = 2) in vec4 vColor;
layout(location = 11) flat in uint vDraw;

layout(location = 0) out float outReactive;

void main() {
    outReactive = clamp(baseAlpha(pc.drawBuffer, vDraw, vUvs, vColor.a), 0.0, 1.0);
}
