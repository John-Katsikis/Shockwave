#version 460
#extension GL_EXT_scalar_block_layout : require

// FXAA (RendererAntiAliasing.cpp): finds aliased edges in the tonemapped image by their
// luma contrast, works out which way each runs and where it ends, and samples across it
// by the fraction of the pixel the edge covers. Written from Timothy Lottes' description
// of the algorithm ("FXAA", NVIDIA white paper, 2009, and the FXAA 3.11 quality preset
// with 12 search steps); no code from it is used.
//
// Input and output are display-encoded (sRGB) values: luma and edge contrast are measured
// as the eye sees them, which is what FXAA's thresholds were tuned for. The input is read
// through a UNORM view and the output written through one, so nothing is encoded twice.

layout(set = 0, binding = 0) uniform sampler2D source;   // linear filtering, clamped to the edges

// Must match FxaaPushConstants in RendererAntiAliasing.cpp.
layout(push_constant, scalar) uniform PushConstants {
    vec2 texel;   // 1 / the image size
} pc;

layout(location = 0) out vec4 outColor;

// The reference values beside the algorithm, as its author published them. Change them
// only with fixture evidence (docs/GraphicsVerification.md).
const float kEdgeThreshold = 0.166;   // relative contrast an edge needs
const float kEdgeThresholdMin = 0.0833;   // ...and absolute, so dark noise is left alone
const float kSubpixel = 0.75;   // how much single-pixel detail is softened
const int kSteps = 12;
const float kStepSizes[kSteps] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

float luma(vec3 encoded) { return dot(encoded, vec3(0.299, 0.587, 0.114)); }
float lumaAt(vec2 uv) { return luma(textureLod(source, uv, 0.0).rgb); }
float lumaAt(vec2 uv, ivec2 offset) { return lumaAt(uv + vec2(offset) * pc.texel); }

void main() {
    vec2 uv = gl_FragCoord.xy * pc.texel;
    vec3 center = textureLod(source, uv, 0.0).rgb;
    float m = luma(center);
    float n = lumaAt(uv, ivec2(0, -1)), s = lumaAt(uv, ivec2(0, 1));
    float e = lumaAt(uv, ivec2(1, 0)), w = lumaAt(uv, ivec2(-1, 0));
    float highest = max(max(max(n, s), max(e, w)), m);
    float lowest = min(min(min(n, s), min(e, w)), m);
    float range = highest - lowest;
    if (range < max(kEdgeThresholdMin, highest * kEdgeThreshold)) {
        outColor = vec4(center, 1.0);   // flat: unchanged, bit for bit
        return;
    }

    float ne = lumaAt(uv, ivec2(1, -1)), nw = lumaAt(uv, ivec2(-1, -1));
    float se = lumaAt(uv, ivec2(1, 1)), sw = lumaAt(uv, ivec2(-1, 1));

    // Subpixel blend: how far the centre stands out from its neighbourhood average.
    float average = (2.0 * (n + s + e + w) + (ne + nw + se + sw)) / 12.0;
    float subpixel = clamp(abs(average - m) / range, 0.0, 1.0);
    subpixel = smoothstep(0.0, 1.0, subpixel);
    subpixel = subpixel * subpixel * kSubpixel;

    // Horizontal edge (luma changes vertically) or vertical edge.
    float horizontal = abs(nw + ne - 2.0 * n) + 2.0 * abs(w + e - 2.0 * m) + abs(sw + se - 2.0 * s);
    float vertical = abs(nw + sw - 2.0 * w) + 2.0 * abs(n + s - 2.0 * m) + abs(ne + se - 2.0 * e);
    bool isHorizontal = horizontal >= vertical;

    // Which side of the centre the edge lies on: the neighbour with the steeper change.
    float positive = isHorizontal ? s : e;
    float negative = isHorizontal ? n : w;
    float gradientPositive = abs(positive - m), gradientNegative = abs(negative - m);
    float stepLength = isHorizontal ? pc.texel.y : pc.texel.x;
    float sideLuma;
    float gradient;
    if (gradientPositive >= gradientNegative) {
        sideLuma = positive;
        gradient = gradientPositive;
    } else {
        sideLuma = negative;
        gradient = gradientNegative;
        stepLength = -stepLength;
    }

    // Walk along the edge, on the boundary between the two sides, both ways until the
    // luma there departs from the edge's.
    vec2 edgeUv = uv;
    if (isHorizontal)
        edgeUv.y += stepLength * 0.5;
    else
        edgeUv.x += stepLength * 0.5;
    vec2 along = isHorizontal ? vec2(pc.texel.x, 0.0) : vec2(0.0, pc.texel.y);
    float edgeLuma = (m + sideLuma) * 0.5;
    float threshold = gradient * 0.25;
    vec2 uvP = edgeUv + along, uvN = edgeUv - along;
    float endP = lumaAt(uvP) - edgeLuma, endN = lumaAt(uvN) - edgeLuma;
    bool doneP = abs(endP) >= threshold, doneN = abs(endN) >= threshold;
    for (int i = 1; i < kSteps && !(doneP && doneN); ++i) {
        if (!doneP) {
            uvP += along * kStepSizes[i];
            endP = lumaAt(uvP) - edgeLuma;
            doneP = abs(endP) >= threshold;
        }
        if (!doneN) {
            uvN -= along * kStepSizes[i];
            endN = lumaAt(uvN) - edgeLuma;
            doneN = abs(endN) >= threshold;
        }
    }

    // Distance to each end; the nearer end decides how much of this pixel the edge covers.
    float distP = isHorizontal ? uvP.x - uv.x : uvP.y - uv.y;
    float distN = isHorizontal ? uv.x - uvN.x : uv.y - uvN.y;
    bool nearerP = distP < distN;
    float nearest = min(distP, distN);
    float span = distP + distN;
    // Only blend when the centre is on the side the edge bends towards.
    bool centreBelow = m - edgeLuma < 0.0;
    bool correct = ((nearerP ? endP : endN) < 0.0) != centreBelow;
    float edgeOffset = correct ? 0.5 - nearest / span : 0.0;
    float offset = max(edgeOffset, subpixel);

    vec2 sampleUv = uv;
    if (isHorizontal)
        sampleUv.y += offset * stepLength;
    else
        sampleUv.x += offset * stepLength;
    outColor = vec4(textureLod(source, sampleUv, 0.0).rgb, 1.0);
}
