#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require

// Bindless: every texture in the engine lives in this one array.
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler linearSampler;

// Must match LightData and FrameData in Renderer.cpp.
struct LightData {
    vec3 position;
    uint type;        // 0 directional, 1 point, 2 spot
    vec3 direction;   // where the light shines
    float range;
    vec3 color;       // color times intensity
    float cosOuter;
    float cosInner;
    float pad0, pad1, pad2;
};

layout(buffer_reference, scalar) readonly buffer FrameBuffer {
    vec3 cameraPosition;
    uint lightCount;
    vec3 skyAmbient;
    uint debugView;   // 0 lit, 1 unlit, 2 normals, 3 lighting only
    vec3 groundAmbient;
    float pad;
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

layout(location = 0) out vec4 outColor;

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

void main() {
    vec4 albedo = texture(sampler2D(textures[nonuniformEXT(vTexture)], linearSampler), vUv) * vColor;
    if (vAlphaCutoff >= 0.0 && albedo.a < vAlphaCutoff) discard;

    FrameBuffer frame = pc.frame;
    vec3 n = normalize(vNormal) * (gl_FrontFacing ? 1.0 : -1.0);
    if (frame.debugView == 1u) { outColor = vec4(albedo.rgb + vEmissive, albedo.a); return; }
    if (frame.debugView == 2u) { outColor = vec4(n * 0.5 + 0.5, 1.0); return; }
    if (frame.debugView == 3u) albedo.rgb = vec3(0.8);

    float roughness = clamp(vRoughnessMetallic.x, 0.03, 1.0);
    float metallic = clamp(vRoughnessMetallic.y, 0.0, 1.0);
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
        if (l.type == 0u) {
            toLight = -l.direction;
        } else {
            vec3 offset = l.position - vWorldPosition;
            float distance = length(offset);
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
        vec3 h = normalize(toLight + v);
        float nDotH = max(dot(n, h), 0.0);
        vec3 f = fresnelSchlick(max(dot(v, h), 0.0), f0);
        // Scaled by PI so intensity 1 lights a white surface facing the light to 1.0.
        vec3 specular = distributionGGX(nDotH, a) * visibilitySmith(nDotV, nDotL, a) * f * PI;
        vec3 diffuse = (1.0 - f) * diffuseColor;
        light += (diffuse + specular) * l.color * (nDotL * attenuation);
    }

    // Hemisphere ambient: sky from above, ground from below.
    vec3 ambient = mix(frame.groundAmbient, frame.skyAmbient, n.y * 0.5 + 0.5);
    light += ambient * (diffuseColor + f0 * 0.25);

    outColor = vec4(light + vEmissive, albedo.a);
}
