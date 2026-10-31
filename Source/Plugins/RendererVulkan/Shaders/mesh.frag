#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_GOOGLE_include_directive : require

// Bindless: every texture in the engine lives in one array (textures, in draw.glsl), and
// materials pick their samplers from another.
#define DRAW_SAMPLING
#include "draw.glsl"
layout(set = 0, binding = 1) uniform sampler linearSampler;
// Depth comparison for shadow maps (reverse-Z: lit when the surface is at least as close).
layout(set = 0, binding = 2) uniform samplerShadow shadowSampler;
// Image-based lighting (RendererEnvironment.cpp): reflections, one roughness per mip
// level, and the light a surface facing each way receives from the surroundings.
layout(set = 0, binding = 4) uniform textureCube specularCube;
layout(set = 0, binding = 5) uniform textureCube irradianceCube;

#include "frame.glsl"
#include "screen.glsl"
#include "reflection.glsl"
#include "clusters.glsl"
#include "fog.glsl"

layout(push_constant, scalar) uniform PushConstants {
    DrawBuffer drawBuffer;
    FrameBuffer frame;
} pc;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec4 vUvs;   // UV0 (xy) and UV1 (zw), tiled
layout(location = 2) in vec4 vColor;
layout(location = 3) flat in uint vTexture;
layout(location = 4) flat in float vAlphaCutoff;
layout(location = 5) in vec3 vWorldPosition;
layout(location = 6) flat in vec2 vRoughnessMetallic;
layout(location = 7) flat in vec3 vEmissive;
layout(location = 8) in vec4 vTangent;
layout(location = 9) flat in uvec4 vMaps;   // normal, metallic-roughness, emissive, occlusion; 0: none
layout(location = 10) flat in vec2 vMapScales;   // normal scale, occlusion strength
layout(location = 11) flat in uint vDraw;
layout(location = 12) in float vLayerMask;
layout(location = 13) in vec2 vLightmapUv;

layout(location = 0) out vec4 outColor;

// False for solid surfaces' pipelines (RendererPipelines.cpp): without a discard the GPU can
// test depth before shading, so surfaces hidden behind nearer ones cost almost nothing.
layout(constant_id = 0) const bool kAlphaTest = true;
// True only for the pipelines that shade over the surface prepass (G03), with its depth and
// EQUAL test: those surfaces are in the ambient occlusion image. Everything else (blended
// surfaces, frames without the prepass) is the shader as it was, bit for bit.
layout(constant_id = 1) const bool kReceivesAo = false;

const float PI = 3.14159265;

// GGX / Trowbridge-Reitz normal distribution.
float distributionGGX(float nDotH, float a) {
    float a2 = a * a;
    float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Smith visibility, height-correlated approximation (already divided by 4 n.l n.v).
float visibilitySmith(float nDotV, float nDotL, float a) {
    float gv = nDotL * (nDotV * (1.0 - a) + a);
    float gl = nDotV * (nDotL * (1.0 - a) + a);
    return 0.5 / max(gv + gl, 1e-5);
}

vec3 fresnelSchlick(float vDotH, vec3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - vDotH, 5.0);
}

// Smooth window so point and spot lights reach exactly zero at their range.
float rangeFalloff(float distance, float range) {
    float ratio = distance / range;
    float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    return window * window / (distance * distance + 1.0);
}

// The diffuse light this surface receives from its surroundings, as irradiance / pi (G05,
// G06): its baked lightmap, its probe sample, or the environment's irradiance cube. All
// three use the same convention, so whichever it is feeds the same terms below.
vec3 surroundingsIrradiance(vec3 n) {
    DrawData draw = pc.drawBuffer.draws[vDraw];
    if (draw.indirectSource == kIndirectLightmap) {
        // Level 0 only: smaller levels would blend neighbouring charts into each other. Kept
        // half a texel inside the atlas so bilinear filtering never wraps around it.
        vec2 size = vec2(textureSize(sampler2D(textures[nonuniformEXT(draw.lightmapTexture)], linearSampler), 0));
        vec2 uv = clamp(vLightmapUv, 0.5 / size, 1.0 - 0.5 / size);
        return textureLod(sampler2D(textures[nonuniformEXT(draw.lightmapTexture)], linearSampler), uv, 0.0).rgb;
    }
    if (draw.indirectSource == kIndirectProbes) {
        ProbeBuffer probes = ProbeBuffer(draw.probes);
        float y[9] = float[](
            0.282095, 0.488603 * n.y, 0.488603 * n.z, 0.488603 * n.x, 1.092548 * n.x * n.y, 1.092548 * n.y * n.z,
            0.315392 * (3.0 * n.z * n.z - 1.0), 1.092548 * n.x * n.z, 0.546274 * (n.x * n.x - n.y * n.y));
        vec3 sum = vec3(0.0);
        for (int i = 0; i < 9; ++i) sum += probes.probe.sh[i].rgb * y[i];
        return max(sum, vec3(0.0));
    }
    return textureLod(samplerCube(irradianceCube, linearSampler), n, 0.0).rgb;
}

// Per-pixel noise, to rotate the filter disk so few samples look smooth rather than banded.
float interleavedGradientNoise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// How much of a light reaches this point through one shadow map: 1 lit .. 0 shadowed.
// Many depth comparisons spread over a rotated disk (each one bilinear), for soft edges.
float filterShadow(
    FrameBuffer frame, uint atlas, uint viewIndex, vec3 worldPos, vec3 n, vec3 toLight, float lightDistance,
    float radius, float bias) {
    ShadowView s = ShadowViews(frame.shadowViews).views[viewIndex];
    float texel = s.texelSize * (s.perspective != 0u ? lightDistance : 1.0);
    // Normal offset: look the point up a little off the surface, more at grazing angles,
    // so a surface doesn't shadow itself (acne).
    float nDotL = clamp(dot(n, toLight), 0.0, 1.0);
    vec3 p = worldPos + n * (texel * bias * (1.5 - nDotL)) + toLight * (texel * bias * 0.5);
    vec4 clip = s.viewProjection * vec4(p, 1.0);
    if (clip.w <= 0.0) return 1.0;
    vec3 ndc = clip.xyz / clip.w;
    if (any(greaterThan(abs(ndc.xy), vec2(1.0))) || ndc.z <= 0.0 || ndc.z > 1.0) return 1.0;

    vec2 atlasTexel = 1.0 / vec2(textureSize(sampler2DShadow(textures[atlas], shadowSampler), 0));
    vec2 center = s.rect.xy + (ndc.xy * 0.5 + 0.5) * s.rect.zw;
    vec2 lo = s.rect.xy + atlasTexel * 0.5, hi = s.rect.xy + s.rect.zw - atlasTexel * 0.5;
    float angle = interleavedGradientNoise(gl_FragCoord.xy) * 6.2831853;
    uint taps = max(frame.shadowTaps, 1u);
    float lit = 0.0;
    for (uint i = 0u; i < taps; ++i) {
        // Vogel (golden angle) disk: evenly spread for any number of samples.
        float r = sqrt((float(i) + 0.5) / float(taps)) * radius;
        float theta = float(i) * 2.39996323 + angle;
        vec2 uv = clamp(center + vec2(cos(theta), sin(theta)) * r * atlasTexel, lo, hi);
        lit += texture(sampler2DShadow(textures[atlas], shadowSampler), vec3(uv, ndc.z));
    }
    return lit / float(taps);
}

// The sun's cascade for a point: the first whose slice of the view reaches past it.
uint sunCascade(FrameBuffer frame, vec3 worldPos, out float depth) {
    depth = dot(worldPos - frame.cameraPosition, frame.cameraForward);
    uint cascade = 0u;
    while (cascade + 1u < frame.cascadeCount && depth > frame.cascadeEnds[cascade]) cascade++;
    return cascade;
}

// How much of a light gets through to this point. The sun picks its cascade by distance
// along the view and blends into the next near the end of each (and out at the shadow
// distance), so changes in sharpness don't show as lines. Spot lights have one map; point
// lights six, one per cube face (+X, -X, +Y, -Y, +Z, -Z).
float lightShadow(FrameBuffer frame, LightData l, vec3 worldPos, vec3 n, vec3 toLight, float distance) {
    uint atlas = frame.localAtlas;
    uint first = l.shadowView, second = 0xFFFFFFFFu;
    float blend = 1.0, fade = 1.0;
    if (l.type == 0u) {
        float depth;
        uint cascade = sunCascade(frame, worldPos, depth);
        if (depth >= frame.shadowDistance) return 1.0;
        atlas = frame.cascadeAtlas;
        first = l.shadowView + cascade;
        float start = cascade == 0u ? 0.0 : frame.cascadeEnds[cascade - 1u];
        float end = frame.cascadeEnds[cascade];
        blend = clamp((end - depth) / max((end - start) * 0.15, 1e-3), 0.0, 1.0);
        if (cascade + 1u < frame.cascadeCount) {
            if (blend < 1.0) second = first + 1u;
        } else {
            fade = blend;
            blend = 1.0;
        }
    } else if (l.type == 1u) {
        vec3 v = worldPos - l.position;
        vec3 a = abs(v);
        first += a.x >= a.y && a.x >= a.z ? (v.x > 0.0 ? 0u : 1u) : a.y >= a.z ? (v.y > 0.0 ? 2u : 3u) : (v.z > 0.0 ? 4u : 5u);
    }
    // One filter call site (looped), which keeps the shader small.
    float result = 1.0;
    for (uint k = 0u; k < 2u; ++k) {
        uint view = k == 0u ? first : second;
        if (view == 0xFFFFFFFFu) break;
        float s = filterShadow(frame, atlas, view, worldPos, n, toLight, distance, l.shadowSoftness, l.shadowBias);
        result = k == 0u ? s : mix(s, result, blend);
    }
    return mix(1.0, result, fade);
}

// A material map through its slot's UV set, transform and sampler.
vec4 sampleMap(uint map, uint slot) {
    SlotBinding binding = pc.drawBuffer.draws[vDraw].slots[slot];
    return sampleSlot(map, binding, slotUv(binding, vUvs));
}

// The tangent frame of the normal map's UVs from screen-space derivatives, for maps whose
// UVs the vertex tangents don't describe (another UV set, rotated, mirrored). t follows +u;
// b points up the texture (-v, glTF's convention), whichever way the UVs are mirrored.
// False where the UVs or the surface are degenerate on screen: the map is left out there.
bool derivativeTangents(vec3 n, vec2 uv, out vec3 t, out vec3 b) {
    vec3 dpdx = dFdx(vWorldPosition), dpdy = dFdy(vWorldPosition);
    vec2 duvdx = dFdx(uv), duvdy = dFdy(uv);
    float det = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    if (abs(det) < 1e-12) return false;
    // Solve dp = dP/du du + dP/dv dv for dP/du and dP/dv.
    vec3 dpdu = (dpdx * duvdy.y - dpdy * duvdx.y) / det;
    vec3 dpdv = (dpdy * duvdx.x - dpdx * duvdy.x) / det;
    t = dpdu - n * dot(n, dpdu);
    float t2 = dot(t, t);
    if (t2 < 1e-20) return false;
    t *= inversesqrt(t2);
    b = cross(n, t);
    if (dot(b, dpdv) > 0.0) b = -b;   // up the texture is -v
    return true;
}

// A normal map's tangent frame at this pixel: the vertex tangents', or (derivative) one built
// from the screen derivatives of its UVs. False where there's none to be had.
bool tangentFrame(vec3 n, vec2 uv, bool derivative, out vec3 t, out vec3 b) {
    if (derivative) return derivativeTangents(n, uv, t, b);
    t = vTangent.xyz - n * dot(n, vTangent.xyz);
    float t2 = dot(t, t);
    if (t2 <= 1e-12) return false;
    t *= inversesqrt(t2);
    b = cross(n, t) * (vTangent.w < 0.0 ? -1.0 : 1.0);
    return true;
}

// How far into the mip chain a map is read, 0..1: specular anti-aliasing (Toksvig) only
// trusts a shortened normal where the GPU has averaged several texels into it, not where
// 8-bit or BC7 rounding shortens it by a hair at full size.
float filteredShare(uint map, SlotBinding binding, vec2 uv) {
    return clamp(textureQueryLod(
        sampler2D(textures[nonuniformEXT(map)], materialSamplers[nonuniformEXT(slotSampler(binding))]), uv).y, 0.0, 1.0);
}

// Reoriented normal mapping (Barre-Brisebois and Hill 2012): detail d on base b, both unit
// tangent-space normals. Where b points straight back, d is turned half a turn about x.
// Must match composeNormals in Assets/Material.cpp.
vec3 composeNormals(vec3 b, vec3 d) {
    vec3 t = b + vec3(0.0, 0.0, 1.0);
    if (t.z <= 1e-4) return vec3(d.x, -d.y, -d.z);
    vec3 u = d * vec3(-1.0, -1.0, 1.0);
    return normalize(t * (dot(t, u) / t.z) - u);
}

// Toksvig 2005 through the Blinn-Phong exponent equivalent to GGX alpha: a shortened mean
// normal widens the highlight. Must match toksvigAlpha in Assets/Material.cpp.
float toksvigAlpha(float alpha, float len) {
    if (!(len < 1.0)) return alpha;
    if (len <= 0.0) return 1.0;
    float a2 = max(alpha * alpha, 1e-7);
    float s = 2.0 / a2 - 2.0;
    float ft = len / (len + s * (1.0 - len));
    return sqrt(2.0 / (ft * s + 2.0));
}

// One normal map's tilt of n, in world space. len: the decoded normal's length, where the
// GPU has filtered it (1 elsewhere), for Toksvig.
vec3 mappedNormal(vec3 n, uint map, SlotBinding binding, bool derivative, float scale, out float len) {
    len = 1.0;
    vec2 uv = slotUv(binding, vUvs);
    vec3 t, b;
    if (!tangentFrame(n, uv, derivative, t, b)) return n;
    vec3 m = sampleSlot(map, binding, uv).xyz * 2.0 - 1.0;
    len = mix(1.0, min(length(m) / 0.996, 1.0), filteredShare(map, binding, uv));
    m.xy *= scale;
    return normalize(t * m.x + b * m.y + n * max(m.z, 1e-3));
}

// The surface normal, tilted by the normal map (tangent space, glTF's convention: green
// points up the texture), not yet turned around for back faces. len as mappedNormal.
vec3 surfaceNormal(out float len) {
    vec3 n = normalize(vNormal);
    len = 1.0;
    if (vMaps.x != 0u) {
        bool derivative = (pc.drawBuffer.draws[vDraw].flags & kDerivativeTangents) != 0u;
        n = mappedNormal(n, vMaps.x, pc.drawBuffer.draws[vDraw].slots[kNormal], derivative, vMapScales.x, len);
    }
    return n;
}

// This pixel's ambient occlusion from the half-resolution image (G04): a joint bilateral
// upsample, the four nearest texels weighted by distance, by how close each one's surface
// is to this one's view distance and by how much their normals agree, so occlusion doesn't
// bleed across silhouettes. The normal is the geometric one, as the prepass stored it.
float ambientOcclusion(FrameBuffer frame, vec3 geometricNormal) {
    uint ao = frame.aoTexture;
    ivec2 size = textureSize(sampler2D(textures[ao], linearSampler), 0);
    vec3 viewPosition = (frame.view * vec4(vWorldPosition, 1.0)).xyz;
    float distance = -viewPosition.z;
    vec3 normal = normalize(mat3(frame.view) * geometricNormal);
    // Half-resolution texel i covers full-resolution pixels 2i and 2i + 1.
    vec2 p = gl_FragCoord.xy * 0.5 - 0.5;
    ivec2 base = ivec2(floor(p));
    vec2 f = p - vec2(base);
    float sum = 0.0, weights = 0.0;
    float nearest = 1.0, nearestGap = 1e30;
    for (int i = 0; i < 4; ++i) {
        ivec2 o = ivec2(i & 1, i >> 1);
        vec4 t = texelFetch(sampler2D(textures[ao], linearSampler), clamp(base + o, ivec2(0), size - 1), 0);
        if (t.y <= 0.0) continue;   // background
        float gap = abs(t.y - distance);
        if (gap < nearestGap) { nearestGap = gap; nearest = t.x; }
        float bilinear = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        float depthWeight = 1.0 / (1.0 + gap / (0.02 * distance + 0.01));
        float normalWeight = pow(clamp(dot(octDecode(t.zw), normal), 0.0, 1.0), 8.0);
        float w = max(bilinear, 1e-3) * depthWeight * depthWeight * normalWeight;
        sum += t.x * w;
        weights += w;
    }
    // No texel agrees (a thin surface in front of everything around it): the closest in depth.
    return weights > 1e-4 ? sum / weights : nearest;
}

float maskChannel(vec4 v, uint channel) {
    return channel == 0u ? v.r : channel == 1u ? v.g : channel == 2u ? v.b : v.a;
}

// One light's direct contribution at this fragment, zero where it doesn't reach: the same
// maths whether the light came from the loop over every light or a cluster's list (G11).
vec3 directLight(FrameBuffer frame, uint i, vec3 n, vec3 v, float nDotV, float a, vec3 f0, vec3 diffuseColor) {
    LightData l = frame.lights[i];
    vec3 toLight;
    float attenuation = 1.0;
    float distance = 1.0;
    if (l.type == 0u) {
        toLight = -l.direction;
    } else {
        vec3 offset = l.position - vWorldPosition;
        distance = length(offset);
        if (distance >= l.range) return vec3(0.0);
        toLight = offset / max(distance, 1e-4);
        attenuation = rangeFalloff(distance, l.range);
        if (l.type == 2u) {
            float cosAngle = dot(-toLight, l.direction);
            attenuation *= smoothstep(l.cosOuter, l.cosInner, cosAngle);
        }
    }
    float nDotL = dot(n, toLight);
    if (nDotL <= 0.0 || attenuation <= 0.0) return vec3(0.0);
    if (l.shadowView != 0xFFFFFFFFu) {
        float shadow = lightShadow(frame, l, vWorldPosition, n, toLight, distance);
        attenuation *= mix(1.0, shadow, l.shadowStrength);
        if (attenuation <= 0.0) return vec3(0.0);
    }
    vec3 h = normalize(toLight + v);
    float nDotH = max(dot(n, h), 0.0);
    vec3 f = fresnelSchlick(max(dot(v, h), 0.0), f0);
    // Scaled by PI so intensity 1 lights a white surface facing the light to 1.0.
    vec3 specular = distributionGGX(nDotH, a) * visibilitySmith(nDotV, nDotL, a) * f * PI;
    vec3 diffuse = (1.0 - f) * diffuseColor;
    return (diffuse + specular) * l.color * (nDotL * attenuation);
}

// The cluster view's colours (DebugView::Clusters): dark blue for no lights, then blue,
// green, yellow and red at 32 or more.
vec3 clusterHeat(uint lights) {
    if (lights == 0u) return vec3(0.02, 0.02, 0.12);
    float t = clamp(float(lights) / 32.0, 0.0, 1.0);
    return clamp(vec3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)), 0.0, 1.0);
}

void main() {
    SlotBinding baseBinding = pc.drawBuffer.draws[vDraw].slots[kBaseColor];
    vec4 albedo = sampleSlot(vTexture, baseBinding, slotUv(baseBinding, vUvs)) * vColor;
    if (kAlphaTest && vAlphaCutoff >= 0.0 && albedo.a < vAlphaCutoff) discard;

    FrameBuffer frame = pc.frame;
    float normalLength;
    vec3 n = surfaceNormal(normalLength);
    vec2 roughnessMetallic = vRoughnessMetallic;
    if (vMaps.y != 0u) roughnessMetallic *= sampleMap(vMaps.y, kMetallicRoughness).gb;   // glTF: green roughness, blue metallic

    // Detail and second layers (A02): mixed into the surface before a single evaluation of
    // the lighting. Alpha, emission and occlusion stay the first layer's.
    uvec2 layerAddress = pc.drawBuffer.draws[vDraw].layers;
    if (layerAddress != uvec2(0u)) {
        LayerBuffer layers = LayerBuffer(layerAddress);
        uint flags = layers.layer.flags;
        vec3 geometric = normalize(vNormal);
        if ((flags & kLayerSecond) != 0u) {
            uint source = layers.layer.layerMaskSource;
            float mask = vLayerMask;
            if (source < 4u) mask = maskChannel(sampleSlot(layers.layer.layerMask, layers.layer.slots[kLayerMaskSlot],
                slotUv(layers.layer.slots[kLayerMaskSlot], vUvs)), source);
            mask = clamp(mask, 0.0, 1.0);
            SlotBinding colorBinding = layers.layer.slots[kLayerBaseColor];
            vec3 layerColor = sampleSlot(layers.layer.layerBaseColor, colorBinding, slotUv(colorBinding, vUvs)).rgb *
                              layers.layer.layerBaseColorFactor.rgb;
            albedo.rgb = mix(albedo.rgb, layerColor, mask);
            vec2 layerRm = vec2(layers.layer.layerRoughness, layers.layer.layerMetallic);
            SlotBinding rmBinding = layers.layer.slots[kLayerMetallicRoughness];
            layerRm *= sampleSlot(layers.layer.layerMetallicRoughness, rmBinding, slotUv(rmBinding, vUvs)).gb;
            roughnessMetallic = mix(roughnessMetallic, layerRm, mask);
            vec3 layerNormal = geometric;
            float layerLength = 1.0;
            if (layers.layer.layerNormal != 0u)
                layerNormal = mappedNormal(geometric, layers.layer.layerNormal, layers.layer.slots[kLayerNormal],
                    (flags & kLayerSecondDerivativeTangents) != 0u, layers.layer.layerNormalScale, layerLength);
            vec3 mixed = mix(n, layerNormal, mask);
            float mixedLength = length(mixed);
            n = mixedLength > 1e-6 ? mixed / mixedLength : geometric;
            normalLength = mix(normalLength, layerLength, mask);
        }
        if ((flags & kLayerDetail) != 0u) {
            float weight = layers.layer.detailStrength;
            SlotBinding maskBinding = layers.layer.slots[kDetailMask];
            weight *= maskChannel(sampleSlot(layers.layer.detailMask, maskBinding, slotUv(maskBinding, vUvs)),
                layers.layer.detailMaskChannel);
            vec2 fade = layers.layer.detailFade;
            if (fade.y > 0.0) {
                float distance = length(frame.cameraPosition - vWorldPosition);
                weight *= 1.0 - (fade.y > fade.x ? smoothstep(fade.x, fade.y, distance) : step(fade.y, distance));
            }
            weight = clamp(weight, 0.0, 1.0);
            // Colour multiplies around neutral white.
            SlotBinding colorBinding = layers.layer.slots[kDetailColor];
            vec3 detailColor = sampleSlot(layers.layer.detailColor, colorBinding, slotUv(colorBinding, vUvs)).rgb;
            albedo.rgb *= mix(vec3(1.0), detailColor, weight);
            roughnessMetallic.x = mix(roughnessMetallic.x, layers.layer.detailRoughness,
                weight * layers.layer.detailRoughnessStrength);
            uint detailMap = layers.layer.detailNormal;
            SlotBinding normalBinding = layers.layer.slots[kDetailNormal];
            vec2 uv = slotUv(normalBinding, vUvs);
            vec3 t, b;
            if (detailMap != 0u && weight > 0.0 &&
                tangentFrame(geometric, uv, (flags & kLayerDetailDerivativeTangents) != 0u, t, b)) {
                vec3 d = sampleSlot(detailMap, normalBinding, uv).xyz * 2.0 - 1.0;
                float detailLength = mix(1.0, min(length(d) / 0.996, 1.0), filteredShare(detailMap, normalBinding, uv));
                // Strength scales the slopes; the composition works on unit normals.
                d.xy *= layers.layer.detailNormalScale * weight;
                d.z = max(d.z, 1e-3);
                d = normalize(d);
                vec3 based = vec3(dot(n, t), dot(n, b), dot(n, geometric));   // the surface so far, in the detail's frame
                vec3 r = composeNormals(based, d);
                n = normalize(t * r.x + b * r.y + geometric * r.z);
                normalLength *= mix(1.0, detailLength, weight);
            }
        }
    }
    // Decals (G08): what the decal pass composed at this pixel, applied once to the surface
    // before lighting: base (1 - coverage) + premultiplied. Only opaque surfaces shaded over
    // the prepass (kReceivesAo) are in the decal buffers; blended ones never take decals.
    vec4 decalColor = vec4(0.0), decalMaterial = vec4(0.0);
    if (kReceivesAo && frame.decalAlbedo != 0u) {
        ivec2 pixel = ivec2(gl_FragCoord.xy);
        decalColor = texelFetch(sampler2D(textures[frame.decalAlbedo], linearSampler), pixel, 0);
        decalMaterial = texelFetch(sampler2D(textures[frame.decalMaterial], linearSampler), pixel, 0);
        albedo.rgb = albedo.rgb * (1.0 - decalColor.a) + decalColor.rgb;
        roughnessMetallic = roughnessMetallic * (1.0 - decalMaterial.a) + decalMaterial.rg;
    }
    if (frame.debugView == 8u) {
        // Decal coverage: red for colour, green for roughness and metallic, over the darkened surface.
        if (!kReceivesAo && kAlphaTest && albedo.a < 0.5) discard;
        outColor = vec4(albedo.rgb * 0.25 * (1.0 - max(decalColor.a, decalMaterial.a)) +
                        vec3(decalColor.a, decalMaterial.a, 0.0), 1.0);
        return;
    }
    n = gl_FrontFacing ? n : -n;
    bool receivesAo = kReceivesAo && frame.aoTexture != 0u;
    float ssao = 1.0;
    if (receivesAo) {
        vec3 geometricNormal = normalize(vNormal);
        ssao = ambientOcclusion(frame, gl_FrontFacing ? geometricNormal : -geometricNormal);
    }
    if (frame.debugView == 5u) {
        // Ambient occlusion alone, white where there's none; blended surfaces aren't in it.
        if (receivesAo) outColor = vec4(vec3(ssao), 1.0);
        else if (kAlphaTest) discard;
        else outColor = vec4(1.0);
        return;
    }
    vec3 emissive = vEmissive;
    if (vMaps.z != 0u) emissive *= sampleMap(vMaps.z, kEmissive).rgb;
    if (frame.debugView == 1u) { outColor = vec4(albedo.rgb + emissive, albedo.a); return; }
    if (frame.debugView == 6u) { outColor = vec4(surroundingsIrradiance(n), 1.0); return; }
    if (frame.debugView == 2u) { outColor = vec4(n * 0.5 + 0.5, 1.0); return; }
    if (frame.debugView == 7u) {
        // Which local reflection probes this pixel reads (G07): a colour per probe times its
        // share, dark grey for what's left to the global environment.
        vec3 R = reflect(-normalize(frame.cameraPosition - vWorldPosition), n);
        uint picked = pc.drawBuffer.draws[vDraw].reflections;
        vec2 shares = vec2(0.0);
        vec3 lookups[2];
        uint slots[2];
        if (picked != 0u) shares = reflectionShares(frame, picked, vWorldPosition, R, lookups, slots);
        const vec3 colors[6] = vec3[](
            vec3(1.0, 0.3, 0.2), vec3(0.2, 0.8, 1.0), vec3(1.0, 0.85, 0.2), vec3(0.6, 0.35, 1.0),
            vec3(0.3, 1.0, 0.4), vec3(1.0, 0.4, 0.8));
        vec3 shown = vec3(0.05) * (1.0 - min(shares.x + shares.y, 1.0));
        if (picked != 0u) {
            shown += shares.x * colors[((picked & 0xFFFFu) - 1u) % 6u];
            if ((picked >> 16) != 0u) shown += shares.y * colors[((picked >> 16) - 1u) % 6u];
        }
        outColor = vec4(shown, 1.0);
        return;
    }
    if (frame.debugView >= 3u) albedo.rgb = vec3(0.8);
    uint cascade = 99u;
    if (frame.debugView == 4u && frame.cascadeCount > 0u) {
        float depth;
        uint c = sunCascade(frame, vWorldPosition, depth);
        if (depth < frame.shadowDistance) cascade = c;
    }

    float roughness = clamp(roughnessMetallic.x, 0.03, 1.0);
    float metallic = clamp(roughnessMetallic.y, 0.0, 1.0);
    float a = roughness * roughness;
    if (normalLength < 1.0) {
        // Normal map mips that average disagreeing normals widen the highlight to match.
        a = toksvigAlpha(a, normalLength);
        roughness = sqrt(a);
    }
    vec3 v = normalize(frame.cameraPosition - vWorldPosition);
    float nDotV = max(dot(n, v), 1e-4);
    vec3 f0 = mix(vec3(0.04), albedo.rgb, metallic);
    vec3 diffuseColor = albedo.rgb * (1.0 - metallic);

    // Directional lights, then point and spot lights: from this pixel's cluster when there
    // are lists (G11), every one of them otherwise. Both add the same lights in table order.
    vec3 light = vec3(0.0);
    for (uint i = 0u; i < frame.directionalCount; ++i) light += directLight(frame, i, n, v, nDotV, a, f0, diffuseColor);
    uint list = clusterListStart(frame, gl_FragCoord.xy, -(frame.view * vec4(vWorldPosition, 1.0)).z);
    ClusterLists lists = ClusterLists(frame.clusters);
    uint listed = list != kNoCluster ? lists.words[list] : kClusterOverflow;
    if (frame.debugView == 11u) {
        vec3 shown = list == kNoCluster                     ? vec3(0.35)
                     : (listed & kClusterOverflow) != 0u ? vec3(1.0, 0.0, 1.0)
                                                         : clusterHeat(listed);
        outColor = vec4(shown, 1.0);
        return;
    }
    if ((listed & kClusterOverflow) != 0u) {
        for (uint i = frame.directionalCount; i < frame.lightCount; ++i)
            light += directLight(frame, i, n, v, nDotV, a, f0, diffuseColor);
    } else {
        for (uint k = 0u; k < listed; ++k) {
            uint i = (lists.words[list + 1u + (k >> 2)] >> ((k & 3u) * 8u)) & 0xFFu;
            light += directLight(frame, i, n, v, nDotV, a, f0, diffuseColor);
        }
    }

    // Image-based lighting: light from all around (an environment map, or the sky above and
    // the ground below), reflected by the split-sum approximation, with the multiple
    // scattering energy compensation of Fdez-Aguera 2019 so rough metals don't lose light.
    // The occlusion map only darkens this: direct light has shadows of its own.
    float occlusion = vMaps.w != 0u ? 1.0 + vMapScales.y * (sampleMap(vMaps.w, kOcclusion).r - 1.0) : 1.0;
    const float tableTexel = 1.0 / 128.0;   // kBrdfTableSize; clamped so lookups never wrap
    vec2 brdf = textureLod(
        sampler2D(textures[frame.brdfTable], linearSampler),
        clamp(vec2(nDotV, roughness), vec2(tableTexel * 0.5), vec2(1.0 - tableTexel * 0.5)), 0.0).rg;
    vec3 prefiltered = textureLod(
        samplerCube(specularCube, linearSampler), reflect(-v, n), roughness * (frame.specularLevels - 1.0)).rgb;
    // Local reflection probes (G07) blend over the global reflection; draws without any
    // (and every draw with reflection_probes off) keep it exactly.
    uint pickedProbes = pc.drawBuffer.draws[vDraw].reflections;
    if (pickedProbes != 0u) {
        vec2 shares;
        prefiltered = localReflections(frame, pickedProbes, vWorldPosition, reflect(-v, n), roughness, prefiltered, shares);
    }
    vec3 irradiance = surroundingsIrradiance(n);
    vec3 fresnel = f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - nDotV, 5.0);
    vec3 singleScatter = fresnel * brdf.x + brdf.y;
    float missed = 1.0 - (brdf.x + brdf.y);
    vec3 averageFresnel = f0 + (1.0 - f0) / 21.0;
    vec3 multiScatter = missed * singleScatter * averageFresnel / (1.0 - missed * averageFresnel);
    vec3 diffuseShare = diffuseColor * (1.0 - singleScatter - multiScatter);
    if (receivesAo) {
        // Screen-space ambient occlusion darkens only the light from the surroundings' irradiance
        // (diffuse, and the multiple scattering that comes with it), combined with the material's
        // own by the minimum: min(occlusion, ssao) = occlusion * share. Reflections keep the map's.
        // Written so that no occlusion (ssao 1, share exactly 1) is the same sum as below.
        float share = occlusion > 0.0 ? min(1.0, ssao / occlusion) : 1.0;
        light += (singleScatter * prefiltered + (multiScatter + diffuseShare) * irradiance * share) * occlusion;
    } else {
        light += (singleScatter * prefiltered + (multiScatter + diffuseShare) * irradiance) * occlusion;
    }

    if (frame.debugView == 4u) {
        // Which sun cascade each pixel reads: red, green, blue, yellow from near to far.
        const vec3 tints[4] = vec3[](vec3(1.0, 0.35, 0.35), vec3(0.35, 1.0, 0.35), vec3(0.4, 0.5, 1.0), vec3(1.0, 1.0, 0.3));
        if (cascade < 4u) light *= tints[cascade];
    }
    vec3 radiance = light + emissive;
    // Fog (A06): S + T L. Blended surfaces blend this over the fogged scene behind them,
    // which keeps the fog in front of them once (docs/Graphics/07-MathematicalContracts.md 7.10).
    if (frame.fogMode != kFogNone && (frame.debugView == 0u || frame.debugView == 12u)) {
        vec4 fog = sceneFog(frame, gl_FragCoord.xy, vWorldPosition);
        if (frame.debugView == 12u) { outColor = vec4(vec3(fog.a), 1.0); return; }
        radiance = fog.rgb + fog.a * radiance;
    } else if (frame.debugView == 12u) {
        outColor = vec4(1.0);
        return;
    }
    outColor = vec4(radiance, albedo.a);
}
