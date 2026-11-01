// The scene's surface shading: mesh.frag (the scene pass, the transmission pass) and
// mesh_skin.frag (the skin pass, with MESH_SKIN_PASS: diffuse light and albedo into the
// skin targets, A08) are both this file.
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
#ifdef MESH_SKIN_PASS
// The skin pass (A08): outColor is the diffuse light before the surface's colour (rgb) and
// the fog's transmittance to it (a); outSkin is the colour (rgb) and the receiver id / 255.
layout(location = 1) out vec4 outSkin;
#endif

// False for solid surfaces' pipelines (RendererPipelines.cpp): without a discard the GPU can
// test depth before shading, so surfaces hidden behind nearer ones cost almost nothing.
layout(constant_id = 0) const bool kAlphaTest = true;
// True only for the pipelines that shade over the surface prepass (G03), with its depth and
// EQUAL test: those surfaces are in the ambient occlusion image. Everything else (blended
// surfaces, frames without the prepass) is the shader as it was, bit for bit.
layout(constant_id = 1) const bool kReceivesAo = false;
// True only for the transmission pass's pipelines (A07b): glass drawn over a copy of the
// opaque scene, blending off, its see-through worked out here.
layout(constant_id = 2) const bool kTransmissionPass = false;

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

// ---- Shading families (A07-A09, Renderer/Shading.h, docs/Shading.md) ----
// Set once in main, before the lights: a standard surface without a coat leaves them all
// as they are, and every light is then the maths it always was.
float gCoat = 0.0;   // clearcoat factor; 0: none
float gCoatA = 0.0;   // the coat's GGX alpha
vec3 gCoatN = vec3(0.0, 0.0, 1.0);   // the coat's normal (its own map, or the geometric one)
bool gAnisotropic = false;   // hair: the stretched highlight
vec3 gHairT = vec3(1.0, 0.0, 0.0), gHairB = vec3(0.0, 1.0, 0.0);   // root to tip, and across
float gAlphaT = 0.0, gAlphaB = 0.0;
// Skin diffusion: 0 everything, 1 specular only (the scene pass leaves the diffuse light to
// the skin pass), 2 diffuse only (the skin pass, before the surface's colour).
uint gSkinMode = 0u;

// Schlick with F0 0.04: the coat's air-to-varnish Fresnel (IOR 1.5). Must match
// schlickFresnel in Renderer/Shading.cpp.
float coatFresnel(float cosTheta) {
    float m = 1.0 - clamp(cosTheta, 0.0, 1.0);
    return 0.04 + 0.96 * m * m * m * m * m;
}

// Anisotropic GGX with exact Smith masking (docs/Graphics/05-CharacterShading.md 5.7), already
// divided by 4 n.l n.v. Must match anisotropicSpecular in Renderer/Shading.cpp.
float anisotropicLambda(vec3 w, vec3 n) {
    float c = dot(w, n);
    float x = gAlphaT * dot(w, gHairT), y = gAlphaB * dot(w, gHairB);
    return (sqrt(1.0 + (x * x + y * y) / max(c * c, 1e-8)) - 1.0) * 0.5;
}
float anisotropicSpecular(vec3 n, vec3 h, vec3 l, vec3 v, float nDotL, float nDotV) {
    float x = dot(h, gHairT) / gAlphaT, y = dot(h, gHairB) / gAlphaB, z = dot(h, n);
    float d = x * x + y * y + z * z;
    float D = 1.0 / (PI * gAlphaT * gAlphaB * d * d);
    float G = 1.0 / (1.0 + anisotropicLambda(l, n) + anisotropicLambda(v, n));
    return D * G / max(4.0 * nDotL * nDotV, 1e-6);
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
    vec3 specular;
    if (gAnisotropic) specular = anisotropicSpecular(n, h, toLight, v, nDotL, nDotV) * f * PI;
    else specular = distributionGGX(nDotH, a) * visibilitySmith(nDotV, nDotL, a) * f * PI;
    vec3 diffuse = (1.0 - f) * diffuseColor;
    vec3 surface = gSkinMode == 0u ? diffuse + specular : gSkinMode == 1u ? specular : diffuse;
    vec3 result = surface * l.color * (nDotL * attenuation);
    if (gCoat > 0.0) {
        // Clearcoat (A07a): the base gets what the coat lets through, both ways, and the
        // coat adds its own highlight on its own normal and roughness.
        float coatL = dot(gCoatN, toLight);
        float coatV = max(dot(gCoatN, v), 1e-4);
        result *= (1.0 - gCoat * coatFresnel(coatL)) * (1.0 - gCoat * coatFresnel(coatV));
        if (coatL > 0.0 && gSkinMode != 2u) {
            float coat = distributionGGX(max(dot(gCoatN, h), 0.0), gCoatA) * visibilitySmith(coatV, coatL, gCoatA) *
                         coatFresnel(max(dot(v, h), 0.0)) * PI * gCoat;
            result += coat * l.color * (coatL * attenuation);
        }
    }
    return result;
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

    // The draw's shading family (A07-A09); none for standard surfaces without a coat.
    uvec2 shadingAddress = pc.drawBuffer.draws[vDraw].shading;
    ShadingData sh;
    uint family = 0u, shadingFlags = 0u;
    if (shadingAddress != uvec2(0u)) {
        sh = ShadingBuffer(shadingAddress).shading;
        family = sh.family;
        shadingFlags = sh.flags;
    }
    float transmission = 0.0, thicknessWorld = 0.0;
    if (family == 3u) {
        vec4 tt = sampleSlot(sh.transmissionTexture, sh.slots[kTransmissionSlot], slotUv(sh.slots[kTransmissionSlot], vUvs));
        transmission = clamp(sh.transmission * tt.r, 0.0, 1.0);
        thicknessWorld = max(sh.thickness * tt.g, 0.0);
        // transmission_quality off: blended glass that lets most of the scene through.
        if ((shadingFlags & kShadingSimplifiedGlass) != 0u) albedo.a *= 1.0 - 0.85 * transmission;
    }

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
#ifndef MESH_SKIN_PASS
    if (frame.debugView == 13u && !kTransmissionPass) {
        // Transmission (A07b): the glass's own colours come from the transmission pass;
        // simplified glass (off) is magenta, everything else dimmed.
        outColor = family == 3u ? vec4(0.8, 0.2, 0.8, 1.0) : vec4(albedo.rgb * 0.2, 1.0);
        return;
    }
    if (frame.debugView == 14u) {
        // Shading families as drawn: coats yellow; skin orange with diffusion, brown without;
        // hair cyan with the stretched highlight, blue with the round one; glass green
        // through the transmission pass, magenta simplified; the rest dimmed.
        vec3 shown = albedo.rgb * 0.2;
        if ((shadingFlags & kShadingCoat) != 0u) shown = vec3(1.0, 0.85, 0.2);
        if (family == 1u) shown = (shadingFlags & kShadingSkinDiffusion) != 0u ? vec3(1.0, 0.5, 0.25) : vec3(0.45, 0.25, 0.1);
        if (family == 2u) shown = (shadingFlags & kShadingAnisotropic) != 0u ? vec3(0.2, 1.0, 1.0) : vec3(0.2, 0.3, 1.0);
        if (family == 3u) shown = kTransmissionPass ? vec3(0.2, 1.0, 0.3) : vec3(0.8, 0.2, 0.8);
        outColor = vec4(shown, 1.0);
        return;
    }
#endif
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
    // Dielectrics reflect 4% head-on (IOR 1.5); transmission's IOR sets its own.
    float dielectricF0 = 0.04;
    if (family == 3u) {
        float r = (sh.ior - 1.0) / (sh.ior + 1.0);
        dielectricF0 = r * r;
    }
    vec3 f0 = mix(vec3(dielectricF0), albedo.rgb, metallic);
    vec3 diffuseColor = albedo.rgb * (1.0 - metallic);
    // Glass passes on what it doesn't diffuse (the transmission pass adds it below).
    if (kTransmissionPass && family == 3u) diffuseColor *= 1.0 - transmission;
#ifdef MESH_SKIN_PASS
    // The skin pass: the diffuse light alone, before the surface's colour (A08).
    gSkinMode = 2u;
    diffuseColor = vec3(1.0 - metallic);
#else
    if ((shadingFlags & kShadingSkinDiffusion) != 0u) gSkinMode = 1u;
#endif
    // Clearcoat (A07a): the coat's strength, roughness and normal (its own map, or the
    // geometric normal: the varnish lies smooth over the base's bumps).
    float coatRoughness = 0.0;
    if ((shadingFlags & kShadingCoat) != 0u) {
        SlotBinding coatBinding = sh.slots[kClearcoatSlot];
        vec4 ct = sampleSlot(sh.clearcoatTexture, coatBinding, slotUv(coatBinding, vUvs));
        gCoat = clamp(sh.clearcoat * ct.r, 0.0, 1.0);
        coatRoughness = clamp(sh.clearcoatRoughness * ct.g, 0.03, 1.0);
        gCoatA = coatRoughness * coatRoughness;
        vec3 geometric = normalize(vNormal);
        vec3 coatNormal = geometric;
        if (sh.clearcoatNormalTexture != 0u) {
            float coatLength;
            coatNormal = mappedNormal(geometric, sh.clearcoatNormalTexture, sh.slots[kClearcoatNormalSlot],
                (shadingFlags & kShadingCoatDerivativeTangents) != 0u, sh.clearcoatNormalScale, coatLength);
        }
        gCoatN = gl_FrontFacing ? coatNormal : -coatNormal;
    }
    // Hair cards (A09): the strands run along the card's u or v, as the vertex tangents
    // describe it. Where they're degenerate the highlight stays round.
    if ((shadingFlags & kShadingAnisotropic) != 0u) {
        vec3 t = vTangent.xyz - n * dot(n, vTangent.xyz);
        if (dot(t, t) > 1e-12) {
            t = normalize(t);
            vec3 b = cross(n, t) * (vTangent.w < 0.0 ? -1.0 : 1.0);   // up the texture (-v)
            gHairT = sh.hairDirection == 0u ? t : -b;
            gHairB = cross(n, gHairT);
            // Must match anisotropicAlphas in Renderer/Shading.cpp.
            float alpha = max(roughness * roughness, 0.0025);
            float anisotropy = clamp(sh.hairAnisotropy, -1.0, 1.0);
            float aspect = sqrt(1.0 - 0.9 * abs(anisotropy));
            gAlphaT = alpha / aspect;
            gAlphaB = alpha * aspect;
            if (anisotropy < 0.0) {
                float swap = gAlphaT;
                gAlphaT = gAlphaB;
                gAlphaB = swap;
            }
            gAnisotropic = true;
        }
    }

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
    vec3 reflected = reflect(-v, n);
    if (gAnisotropic) {
        // The stretched highlight's reflections: the isotropic cube along a normal bent
        // towards the stretch (an approximation; the cube isn't an anisotropic convolution).
        vec3 direction = sh.hairAnisotropy >= 0.0 ? gHairB : gHairT;
        vec3 bent = cross(cross(direction, v), direction);
        if (dot(bent, bent) > 1e-12) reflected = reflect(-v, normalize(mix(n, normalize(bent), abs(sh.hairAnisotropy))));
    }
    vec3 prefiltered = textureLod(
        samplerCube(specularCube, linearSampler), reflected, roughness * (frame.specularLevels - 1.0)).rgb;
    // Local reflection probes (G07) blend over the global reflection; draws without any
    // (and every draw with reflection_probes off) keep it exactly.
    uint pickedProbes = pc.drawBuffer.draws[vDraw].reflections;
    if (pickedProbes != 0u) {
        vec2 shares;
        prefiltered = localReflections(frame, pickedProbes, vWorldPosition, reflected, roughness, prefiltered, shares);
    }
    vec3 irradiance = surroundingsIrradiance(n);
    vec3 fresnel = f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - nDotV, 5.0);
    vec3 singleScatter = fresnel * brdf.x + brdf.y;
    float missed = 1.0 - (brdf.x + brdf.y);
    vec3 averageFresnel = f0 + (1.0 - f0) / 21.0;
    vec3 multiScatter = missed * singleScatter * averageFresnel / (1.0 - missed * averageFresnel);
    vec3 diffuseShare = diffuseColor * (1.0 - singleScatter - multiScatter);
    if (gSkinMode == 1u) diffuseShare = vec3(0.0);   // the skin pass draws it
    vec3 ambient;
    if (gSkinMode == 2u) {
        // The skin pass: the diffuse light from the surroundings alone.
        float share = receivesAo && occlusion > 0.0 ? min(1.0, ssao / occlusion) : 1.0;
        ambient = diffuseShare * irradiance * share * occlusion;
    } else if (receivesAo) {
        // Screen-space ambient occlusion darkens only the light from the surroundings' irradiance
        // (diffuse, and the multiple scattering that comes with it), combined with the material's
        // own by the minimum: min(occlusion, ssao) = occlusion * share. Reflections keep the map's.
        // Written so that no occlusion (ssao 1, share exactly 1) is the same sum as below.
        float share = occlusion > 0.0 ? min(1.0, ssao / occlusion) : 1.0;
        ambient = (singleScatter * prefiltered + (multiScatter + diffuseShare) * irradiance * share) * occlusion;
    } else {
        ambient = (singleScatter * prefiltered + (multiScatter + diffuseShare) * irradiance) * occlusion;
    }
    if (gCoat > 0.0) {
        // The coat's reflection, from its own normal and roughness, over what it lets through
        // (out along the view; in, its average over the hemisphere: 0.04 + 0.96 / 21).
        // Screen-space occlusion doesn't darken it; the occlusion map does, like reflections.
        float coatV = max(dot(gCoatN, v), 1e-4);
        vec2 coatBrdf = textureLod(
            sampler2D(textures[frame.brdfTable], linearSampler),
            clamp(vec2(coatV, coatRoughness), vec2(tableTexel * 0.5), vec2(1.0 - tableTexel * 0.5)), 0.0).rg;
        vec3 coatReflected = reflect(-v, gCoatN);
        vec3 coatPrefiltered = textureLod(
            samplerCube(specularCube, linearSampler), coatReflected, coatRoughness * (frame.specularLevels - 1.0)).rgb;
        if (pickedProbes != 0u) {
            vec2 shares;
            coatPrefiltered = localReflections(
                frame, pickedProbes, vWorldPosition, coatReflected, coatRoughness, coatPrefiltered, shares);
        }
        ambient *= (1.0 - gCoat * coatFresnel(coatV)) * (1.0 - gCoat * (0.04 + 0.96 / 21.0));
        if (gSkinMode != 2u) ambient += gCoat * (0.04 * coatBrdf.x + coatBrdf.y) * coatPrefiltered * occlusion;
    }
    light += ambient;
#ifdef MESH_SKIN_PASS
    {
        // Into the skin targets: the light, the fog's transmittance to the surface (the
        // composite multiplies by it), the colour and which receiver this is.
        float fogT = 1.0;
        if (frame.fogMode != kFogNone && frame.debugView == 0u) fogT = sceneFog(frame, gl_FragCoord.xy, vWorldPosition).a;
        outColor = vec4(light, fogT);
        outSkin = vec4(albedo.rgb, float(min(sh.skinReceiver, 255u)) / 255.0);
        return;
    }
#endif

    // Transmission (A07b): the light through the glass, from the copy of the opaque scene
    // behind it. Thin glass (and low quality) looks straight through; solid glass bends
    // the view by Snell's law and looks where the refracted ray leaves the thickness. Where
    // that's off screen it falls back to the environment, and where something in front of
    // the glass covers that spot, to straight through. Rough glass reads blurrier mips.
    vec3 transmitted = vec3(0.0), transmittedWeight = vec3(0.0);
    uint transmissionFallback = 0u;   // 0 the scene behind, 1 the environment, 2 straight through
    if (kTransmissionPass && family == 3u) {
        transmittedWeight = max(1.0 - singleScatter - multiScatter, vec3(0.0)) * albedo.rgb * transmission;
        vec2 own = gl_FragCoord.xy / frame.renderSize;
        vec2 uv = own;
        vec3 direction = -v;
        if ((shadingFlags & kShadingRefract) != 0u && (shadingFlags & kShadingThin) == 0u) {
            vec3 refracted = refract(-v, n, 1.0 / sh.ior);
            if (dot(refracted, refracted) > 0.0) direction = normalize(refracted);
            vec4 clip = frame.projection * frame.view * vec4(vWorldPosition + direction * thicknessWorld, 1.0);
            if (clip.w <= 1e-6) {
                transmissionFallback = 1u;
            } else {
                uv = screenUvFromClip(clip);
                if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
                    transmissionFallback = 1u;
                } else {
                    ivec2 size = ivec2(frame.renderSize);
                    float behind = texelFetch(sampler2D(textures[frame.sceneDepth], linearSampler),
                        clamp(ivec2(uv * frame.renderSize), ivec2(0), size - 1), 0).r;
                    if (behind > gl_FragCoord.z) {   // reverse-Z: something nearer than the glass
                        transmissionFallback = 2u;
                        uv = own;
                    }
                }
            }
        }
        if (transmissionFallback == 1u) {
            transmitted = textureLod(samplerCube(specularCube, linearSampler), direction,
                roughness * (frame.specularLevels - 1.0)).rgb;
        } else {
            float lod = log2(max(frame.renderSize.x, 1.0)) * roughness * clamp(sh.ior * 2.0 - 2.0, 0.0, 1.0);
            lod = clamp(lod, 0.0, max(frame.transmissionLevels - 1.0, 0.0));
            vec2 texel = 1.0 / frame.renderSize;
            uint background = frame.transmissionBackground;
            transmitted = textureLod(sampler2D(textures[background], linearSampler),
                clamp(uv, texel * 0.5, 1.0 - texel * 0.5), lod).rgb;
            if ((shadingFlags & kShadingRoughTaps) != 0u && lod > 0.5) {
                // High: four more taps around it, turned per pixel, for smoother rough glass.
                float angle = interleavedGradientNoise(gl_FragCoord.xy) * 6.2831853;
                float radius = exp2(lod) * 0.75;
                for (int k = 0; k < 4; ++k) {
                    float theta = angle + float(k) * 1.5707963;
                    vec2 tap = clamp(uv + vec2(cos(theta), sin(theta)) * radius * texel, texel * 0.5, 1.0 - texel * 0.5);
                    transmitted += textureLod(sampler2D(textures[background], linearSampler), tap, lod).rgb;
                }
                transmitted *= 0.2;
            }
        }
        // Absorbed along the way: exactly attenuation_color after attenuation_distance.
        transmitted *= transmittedWeight * exp(-sh.absorption * thicknessWorld);
    }

    if (frame.debugView == 4u) {
        // Which sun cascade each pixel reads: red, green, blue, yellow from near to far.
        const vec3 tints[4] = vec3[](vec3(1.0, 0.35, 0.35), vec3(0.35, 1.0, 0.35), vec3(0.4, 0.5, 1.0), vec3(1.0, 1.0, 0.3));
        if (cascade < 4u) light *= tints[cascade];
    }
    vec3 radiance = light + emissive;
    if (kTransmissionPass && family == 3u && frame.debugView == 13u) {
        // Where the light through the glass came from: green the scene behind, red the
        // environment (off screen), blue straight through (something in front was there).
        const vec3 fallbacks[3] = vec3[](vec3(0.2, 1.0, 0.3), vec3(1.0, 0.25, 0.2), vec3(0.25, 0.4, 1.0));
        outColor = vec4(fallbacks[transmissionFallback] * (0.5 + 0.5 * transmission), 1.0);
        return;
    }
    // Fog (A06): S + T L. Blended surfaces blend this over the fogged scene behind them,
    // which keeps the fog in front of them once (docs/Graphics/07-MathematicalContracts.md 7.10).
    // Glass: the copy behind it is already fogged all the way, so the light through it gets
    // no more; its own surface takes the fog in front, in proportion to what it doesn't pass.
    if (frame.fogMode != kFogNone && (frame.debugView == 0u || frame.debugView == 12u)) {
        vec4 fog = sceneFog(frame, gl_FragCoord.xy, vWorldPosition);
        if (frame.debugView == 12u) { outColor = vec4(vec3(fog.a), 1.0); return; }
        if (kTransmissionPass && family == 3u) {
            outColor = vec4(fog.rgb * (1.0 - transmittedWeight) + fog.a * radiance + transmitted, 1.0);
            return;
        }
        radiance = fog.rgb + fog.a * radiance;
    } else if (frame.debugView == 12u) {
        outColor = vec4(1.0);
        return;
    }
    if (kTransmissionPass && family == 3u) {
        outColor = vec4(radiance + transmitted, 1.0);
        return;
    }
    outColor = vec4(radiance, albedo.a);
}
