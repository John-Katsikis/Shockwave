#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_GOOGLE_include_directive : require

// The surface prepass (G03): depth, and the surface's geometric normal in view space, for
// the screen-space passes after it (ambient occlusion, decals). Drawn with mesh.vert, so the
// opaque shading after it lands on exactly the same depths and tests them for EQUAL. The
// cut of an alpha-tested surface is baseAlpha, the same as the scene's and the shadows'.
// Normal maps aren't applied: occlusion follows the geometry, not its painted detail.
#define DRAW_SAMPLING
#include "draw.glsl"
#include "frame.glsl"
#include "screen.glsl"

layout(push_constant, scalar) uniform PushConstants {
    DrawBuffer drawBuffer;
    FrameBuffer frame;
} pc;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec4 vUvs;
layout(location = 2) in vec4 vColor;
layout(location = 4) flat in float vAlphaCutoff;
layout(location = 11) flat in uint vDraw;

layout(location = 0) out vec2 outNormal;   // octahedral, view space
// The draw's decal receiver layers (G08): only written while decals are on, when the
// prepass has this second target; without it the write goes nowhere.
layout(location = 1) out uint outDecalLayers;

// As in mesh.frag: false for solid surfaces, so the GPU tests depth before shading.
layout(constant_id = 0) const bool kAlphaTest = true;

void main() {
    if (kAlphaTest && vAlphaCutoff >= 0.0 && baseAlpha(pc.drawBuffer, vDraw, vUvs, vColor.a) < vAlphaCutoff) discard;
    vec3 n = normalize(vNormal);
    n = gl_FrontFacing ? n : -n;
    outNormal = octEncode(normalize(mat3(pc.frame.view) * n));
    outDecalLayers = pc.drawBuffer.draws[vDraw].decalLayers;
}
