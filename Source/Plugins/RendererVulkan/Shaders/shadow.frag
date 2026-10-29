#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

// Only alpha-tested surfaces (leaves, fences) need anything here: their cut-out parts
// mustn't cast. Everything else just writes depth. The cut is baseAlpha, the same as the
// scene's, so a shadow's holes line up with what's seen.
#define DRAW_SAMPLING
#include "draw.glsl"

layout(push_constant, scalar) uniform PushConstants {
    DrawBuffer drawBuffer;
} pc;

layout(location = 0) in vec4 vUvs;
layout(location = 1) in float vAlpha;
layout(location = 2) flat in uint vDraw;

void main() {
    float cutoff = pc.drawBuffer.draws[vDraw].alphaCutoff;
    if (cutoff >= 0.0 && baseAlpha(pc.drawBuffer, vDraw, vUvs, vAlpha) < cutoff) discard;
}
