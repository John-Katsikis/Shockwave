#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_GOOGLE_include_directive : require

// The decal pass (G08, docs/GraphicsImplementation.md 12.2): for the visible surface at
// this pixel, as the surface prepass left it, whether it's inside the decal's box, on a
// layer the decal lands on and facing it; if so, the decal's premultiplied colour and
// coverage, blended "over" what earlier decals left (the pipeline's blend). Opaque
// shading then applies the result once: albedo = base (1 - A) + C (mesh.frag).
#include "frame.glsl"
#include "screen.glsl"
#include "decal.glsl"

layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler linearSampler;
layout(set = 1, binding = 0) uniform sampler2D sceneDepth;   // the prepass's, nearest
layout(set = 1, binding = 1) uniform sampler2D surfaceNormals;   // octahedral, view space
layout(set = 1, binding = 2) uniform usampler2D decalLayers;   // the surface's receiver layers

layout(push_constant, scalar) uniform PushConstants {
    FrameBuffer frame;
    DecalBuffer decals;
} pc;

layout(location = 0) flat in uint vDecal;
layout(location = 0) out vec4 outAlbedo;   // premultiplied linear colour, coverage
layout(location = 1) out vec4 outMaterial;   // premultiplied roughness, metallic, -, coverage

void main() {
    DecalData d = pc.decals.decals[vDecal];
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(sceneDepth, pixel, 0).r;
    vec2 uv = (vec2(pixel) + 0.5) / pc.frame.renderSize;
    vec3 viewPosition;
    if (!viewPositionFromDepth(uv, depth, pc.frame.inverseProjection, viewPosition)) discard;   // sky
    // The view is rigid: its inverse is the transpose of its rotation.
    mat3 toWorld = transpose(mat3(pc.frame.view));
    vec3 world = toWorld * (viewPosition - pc.frame.view[3].xyz);
    vec3 local = (d.worldToBox * vec4(world, 1.0)).xyz;
    if (any(greaterThan(abs(local), vec3(0.5)))) discard;
    if ((texelFetch(decalLayers, pixel, 0).r & d.receiverMask) == 0u) discard;
    vec3 normal = toWorld * octDecode(texelFetch(surfaceNormals, pixel, 0).rg);
    float fade = decalAngleFade(d, normal);
    if (fade <= 0.0) discard;

    // u along +X, v down -Y; kept half a texel inside so filtering never reads the far edge.
    vec2 textureUv = vec2(local.x + 0.5, 0.5 - local.y);
    vec2 size = vec2(textureSize(sampler2D(textures[nonuniformEXT(d.texture)], linearSampler), 0));
    textureUv = clamp(textureUv, 0.5 / size, 1.0 - 0.5 / size);
    vec4 texel = texture(sampler2D(textures[nonuniformEXT(d.texture)], linearSampler), textureUv);
    float alpha = clamp(texel.a * d.color.a * fade, 0.0, 1.0);
    outAlbedo = (d.channels & kDecalAlbedo) != 0u ? vec4(texel.rgb * d.color.rgb * alpha, alpha) : vec4(0.0);
    outMaterial = (d.channels & kDecalRoughnessMetallic) != 0u
                      ? vec4(d.roughness * alpha, d.metallic * alpha, 0.0, alpha)
                      : vec4(0.0);
}
