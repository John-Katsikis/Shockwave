#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_scalar_block_layout : require

// Bindless: every texture in the engine lives in this one array.
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler linearSampler;
// Depth comparison for shadow maps (reverse-Z: lit when the surface is at least as close).
layout(set = 0, binding = 2) uniform samplerShadow shadowSampler;
// Image-based lighting (RendererEnvironment.cpp): reflections, one roughness per mip
// level, and the light a surface facing each way receives from the surroundings.
layout(set = 0, binding = 4) uniform textureCube specularCube;
layout(set = 0, binding = 5) uniform textureCube irradianceCube;

// Must match LightData, ShadowView and FrameData in RendererImpl.h.
struct LightData {
    vec3 position;
    uint type;        // 0 directional, 1 point, 2 spot
    vec3 direction;   // where the light shines
    float range;
    vec3 color;       // color times intensity
    float cosOuter;
    float cosInner;
    uint shadowView;  // first shadow map; 0xFFFFFFFF: casts none
    float shadowStrength;
    float shadowSoftness;   // filter radius, texels
    float shadowBias;       // normal offset, texels
    float pad0, pad1, pad2;
};

struct ShadowView {
    mat4 viewProjection;
    vec4 rect;          // uv offset (xy) and scale (zw) in its atlas
    float texelSize;    // world size of a texel (at distance 1 when perspective)
    uint perspective;
    float pad0, pad1;
};

layout(buffer_reference, scalar) readonly buffer ShadowViews {
    ShadowView views[];
};

layout(buffer_reference, scalar) readonly buffer FrameBuffer {
    vec3 cameraPosition;
    uint lightCount;
    vec3 skyAmbient;
    uint debugView;   // 0 lit, 1 unlit, 2 normals, 3 lighting only, 4 shadow cascades
    vec3 groundAmbient;
    uint cascadeCount;
    vec3 cameraForward;
    float shadowDistance;
    vec4 cascadeEnds;   // view depth where each sun cascade ends
    uint cascadeAtlas;
    uint localAtlas;
    uint shadowTaps;
    uint brdfTable;   // bindless slot of image-based lighting's BRDF table
    uvec2 shadowViews;   // a ShadowViews: MoltenVK reads references kept inside buffers as null
    float specularLevels;   // mip levels of specularCube
    uint pad;
    LightData lights[];
};

layout(push_constant, scalar) uniform PushConstants {
    layout(offset = 8) FrameBuffer frame;
} pc;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec4 vColor;
layout(location = 3) flat in uint vTexture;
layout(location = 4) flat in float vAlphaCutoff;
layout(location = 5) in vec3 vWorldPosition;
layout(location = 6) flat in vec2 vRoughnessMetallic;
layout(location = 7) flat in vec3 vEmissive;
layout(location = 8) in vec4 vTangent;
layout(location = 9) flat in uvec4 vMaps;   // normal, metallic-roughness, emissive, occlusion; 0: none
layout(location = 10) flat in vec2 vMapScales;   // normal scale, occlusion strength

layout(location = 0) out vec4 outColor;

// False for solid surfaces' pipelines (RendererPipelines.cpp): without a discard the GPU can
// test depth before shading, so surfaces hidden behind nearer ones cost almost nothing.
layout(constant_id = 0) const bool kAlphaTest = true;

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

vec4 sampleMap(uint map) {
    return texture(sampler2D(textures[nonuniformEXT(map)], linearSampler), vUv);
}

// The surface normal, tilted by the normal map (tangent space, glTF's convention: green
// points up the texture), and turned around on back faces.
vec3 surfaceNormal() {
    vec3 n = normalize(vNormal);
    if (vMaps.x != 0u) {
        vec3 t = vTangent.xyz - n * dot(n, vTangent.xyz);
        float t2 = dot(t, t);
        if (t2 > 1e-12) {
            t *= inversesqrt(t2);
            vec3 b = cross(n, t) * (vTangent.w < 0.0 ? -1.0 : 1.0);
            vec3 m = sampleMap(vMaps.x).xyz * 2.0 - 1.0;
            m.xy *= vMapScales.x;
            n = normalize(t * m.x + b * m.y + n * max(m.z, 1e-3));
        }
    }
    return gl_FrontFacing ? n : -n;
}

void main() {
    vec4 albedo = texture(sampler2D(textures[nonuniformEXT(vTexture)], linearSampler), vUv) * vColor;
    if (kAlphaTest && vAlphaCutoff >= 0.0 && albedo.a < vAlphaCutoff) discard;

    FrameBuffer frame = pc.frame;
    vec3 n = surfaceNormal();
    vec3 emissive = vEmissive;
    if (vMaps.z != 0u) emissive *= sampleMap(vMaps.z).rgb;
    if (frame.debugView == 1u) { outColor = vec4(albedo.rgb + emissive, albedo.a); return; }
    if (frame.debugView == 2u) { outColor = vec4(n * 0.5 + 0.5, 1.0); return; }
    if (frame.debugView >= 3u) albedo.rgb = vec3(0.8);
    uint cascade = 99u;
    if (frame.debugView == 4u && frame.cascadeCount > 0u) {
        float depth;
        uint c = sunCascade(frame, vWorldPosition, depth);
        if (depth < frame.shadowDistance) cascade = c;
    }

    vec2 roughnessMetallic = vRoughnessMetallic;
    if (vMaps.y != 0u) roughnessMetallic *= sampleMap(vMaps.y).gb;   // glTF: green roughness, blue metallic
    float roughness = clamp(roughnessMetallic.x, 0.03, 1.0);
    float metallic = clamp(roughnessMetallic.y, 0.0, 1.0);
    float a = roughness * roughness;
    vec3 v = normalize(frame.cameraPosition - vWorldPosition);
    float nDotV = max(dot(n, v), 1e-4);
    vec3 f0 = mix(vec3(0.04), albedo.rgb, metallic);
    vec3 diffuseColor = albedo.rgb * (1.0 - metallic);

    vec3 light = vec3(0.0);
    for (uint i = 0u; i < frame.lightCount; ++i) {
        LightData l = frame.lights[i];
        vec3 toLight;
        float attenuation = 1.0;
        float distance = 1.0;
        if (l.type == 0u) {
            toLight = -l.direction;
        } else {
            vec3 offset = l.position - vWorldPosition;
            distance = length(offset);
            if (distance >= l.range) continue;
            toLight = offset / max(distance, 1e-4);
            attenuation = rangeFalloff(distance, l.range);
            if (l.type == 2u) {
                float cosAngle = dot(-toLight, l.direction);
                attenuation *= smoothstep(l.cosOuter, l.cosInner, cosAngle);
            }
        }
        float nDotL = dot(n, toLight);
        if (nDotL <= 0.0 || attenuation <= 0.0) continue;
        if (l.shadowView != 0xFFFFFFFFu) {
            float shadow = lightShadow(frame, l, vWorldPosition, n, toLight, distance);
            attenuation *= mix(1.0, shadow, l.shadowStrength);
            if (attenuation <= 0.0) continue;
        }
        vec3 h = normalize(toLight + v);
        float nDotH = max(dot(n, h), 0.0);
        vec3 f = fresnelSchlick(max(dot(v, h), 0.0), f0);
        // Scaled by PI so intensity 1 lights a white surface facing the light to 1.0.
        vec3 specular = distributionGGX(nDotH, a) * visibilitySmith(nDotV, nDotL, a) * f * PI;
        vec3 diffuse = (1.0 - f) * diffuseColor;
        light += (diffuse + specular) * l.color * (nDotL * attenuation);
    }

    // Image-based lighting: light from all around (an environment map, or the sky above and
    // the ground below), reflected by the split-sum approximation, with the multiple
    // scattering energy compensation of Fdez-Aguera 2019 so rough metals don't lose light.
    // The occlusion map only darkens this: direct light has shadows of its own.
    float occlusion = vMaps.w != 0u ? 1.0 + vMapScales.y * (sampleMap(vMaps.w).r - 1.0) : 1.0;
    const float tableTexel = 1.0 / 128.0;   // kBrdfTableSize; clamped so lookups never wrap
    vec2 brdf = textureLod(
        sampler2D(textures[frame.brdfTable], linearSampler),
        clamp(vec2(nDotV, roughness), vec2(tableTexel * 0.5), vec2(1.0 - tableTexel * 0.5)), 0.0).rg;
    vec3 prefiltered = textureLod(
        samplerCube(specularCube, linearSampler), reflect(-v, n), roughness * (frame.specularLevels - 1.0)).rgb;
    vec3 irradiance = textureLod(samplerCube(irradianceCube, linearSampler), n, 0.0).rgb;
    vec3 fresnel = f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - nDotV, 5.0);
    vec3 singleScatter = fresnel * brdf.x + brdf.y;
    float missed = 1.0 - (brdf.x + brdf.y);
    vec3 averageFresnel = f0 + (1.0 - f0) / 21.0;
    vec3 multiScatter = missed * singleScatter * averageFresnel / (1.0 - missed * averageFresnel);
    vec3 diffuseShare = diffuseColor * (1.0 - singleScatter - multiScatter);
    light += (singleScatter * prefiltered + (multiScatter + diffuseShare) * irradiance) * occlusion;

    if (frame.debugView == 4u) {
        // Which sun cascade each pixel reads: red, green, blue, yellow from near to far.
        const vec3 tints[4] = vec3[](vec3(1.0, 0.35, 0.35), vec3(0.35, 1.0, 0.35), vec3(0.4, 0.5, 1.0), vec3(1.0, 1.0, 0.3));
        if (cascade < 4u) light *= tints[cascade];
    }
    outColor = vec4(light + emissive, albedo.a);
}
