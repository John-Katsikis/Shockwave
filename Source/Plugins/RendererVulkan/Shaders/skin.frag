#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_GOOGLE_include_directive : require

// Skin diffusion (A08, RendererShading.cpp, docs/Shading.md): a separable bilateral blur of
// the skin pass's diffuse light, across then down, and the composite that adds it, times
// the skin's colour, to the scene. Only the diffuse light is blurred: highlights, eyes,
// hair and everything that isn't skin stay sharp. Must match Renderer/Shading.h's
// skinKernelWeight and projectedRadius.

layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler linearSampler;

#include "frame.glsl"

// One receiver's spread (the frame's skin profile table), indexed by receiver id.
struct SkinProfile {
    vec3 radius;   // metres per channel, times the strength
    float maxPixels;
};
layout(buffer_reference, scalar) readonly buffer SkinProfiles {
    SkinProfile profiles[];
};

layout(push_constant, scalar) uniform PushConstants {
    FrameBuffer frame;
    SkinProfiles profiles;
    uint lightTexture;   // the diffuse light: rgb, fog transmittance in a
    uint skinTexture;   // colour in rgb, receiver id / 255 in a
    uint depthTexture;   // the scene's depth, reverse-Z
    uint mode;   // 0 blur across, 1 blur down, 2 composite
    uint taps;   // per direction, odd
    uint pad;
} pc;

layout(location = 0) out vec4 outColor;

ivec2 imageSize2() { return textureSize(sampler2D(textures[pc.skinTexture], linearSampler), 0); }
vec4 fetch(uint image, ivec2 p) { return texelFetch(sampler2D(textures[image], linearSampler), p, 0); }
uint receiverAt(ivec2 p) { return uint(fetch(pc.skinTexture, p).a * 255.0 + 0.5); }

// Positive view distance from reverse-Z depth (the contract's inverse projection).
float viewDistance(ivec2 p, ivec2 size) {
    float depth = fetch(pc.depthTexture, p).r;
    if (!(depth > 0.0)) return 1e30;
    vec2 uv = (vec2(p) + 0.5) / vec2(size);
    vec4 h = pc.frame.inverseProjection * vec4(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, depth, 1.0);
    return -(h.z / h.w);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 size = imageSize2();
    vec4 center = fetch(pc.lightTexture, p);
    uint receiver = receiverAt(p);
    if (pc.mode == 2u) {
        // Composite: the blurred diffuse light, times the colour, through the fog in front.
        vec4 skin = fetch(pc.skinTexture, p);
        outColor = receiver == 0u ? vec4(0.0) : vec4(center.a * skin.rgb * center.rgb, 0.0);
        return;
    }
    if (receiver == 0u) {
        outColor = center;
        return;
    }
    SkinProfile profile = pc.profiles.profiles[receiver];
    float z = viewDistance(p, size);
    // Pixels a metre covers at this distance: fy / z for perspective, the orthographic
    // scale otherwise (projection[3][3] is 1 only for orthographic).
    float fy = pc.frame.projection[1][1] * 0.5 * float(size.y);
    bool orthographic = pc.frame.projection[3][3] == 1.0;
    float pixelsPerMetre = orthographic ? fy : fy / max(z, 1e-4);
    vec3 radius = min(profile.radius * pixelsPerMetre, vec3(profile.maxPixels));
    float reach = max(radius.r, max(radius.g, radius.b));
    if (reach < 0.5) {
        outColor = center;   // too small on screen to spread: skipped
        return;
    }
    // Depth: neighbours further than this from the surface's distance belong to something
    // else (a nose in front of a cheek); measured in metres, not reverse-Z.
    float tolerance = 0.01 + 2.0 * max(profile.radius.r, max(profile.radius.g, profile.radius.b));
    ivec2 axis = pc.mode == 0u ? ivec2(1, 0) : ivec2(0, 1);
    uint taps = max(pc.taps, 3u) | 1u;
    // No more taps than pixels in reach, so small spreads don't count a pixel twice.
    int halfTaps = min(int(taps / 2u), int(ceil(reach)));
    vec3 sum = center.rgb, weights = vec3(1.0);   // the centre always counts, weight 1
    vec3 sigma = max(radius * 0.5, vec3(1e-3));
    for (int i = -halfTaps; i <= halfTaps; ++i) {
        if (i == 0) continue;
        float offset = float(i) / float(halfTaps) * reach;
        ivec2 q = p + axis * int(round(offset));
        if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, size)) || q == p) continue;
        if (receiverAt(q) != receiver) continue;   // never across to another character
        float dz = viewDistance(q, size) - z;
        float depthWeight = exp(-dz * dz / (2.0 * tolerance * tolerance));
        vec3 w = exp(-offset * offset / (2.0 * sigma * sigma)) * depthWeight;
        w *= step(vec3(0.5), radius);   // channels that don't spread keep their own light
        sum += w * fetch(pc.lightTexture, q).rgb;
        weights += w;
    }
    outColor = vec4(sum / weights, center.a);
}
