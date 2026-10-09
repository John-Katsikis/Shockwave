#version 460
#extension GL_EXT_nonuniform_qualifier : require

// Bindless: every texture in the engine lives in this one array.
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler linearSampler;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec4 vColor;
layout(location = 3) flat in uint vTexture;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 albedo = texture(sampler2D(textures[nonuniformEXT(vTexture)], linearSampler), vUv) * vColor;

    // Placeholder lighting (one sun plus sky ambient) until the real lighting milestone.
    vec3 n = normalize(vNormal);
    vec3 sunDir = normalize(vec3(0.4, 0.8, 0.3));
    float diffuse = max(dot(n, sunDir), 0.0);
    float sky = 0.5 + 0.5 * n.y;
    vec3 light = vec3(1.0, 0.96, 0.9) * diffuse * 2.2 + vec3(0.35, 0.45, 0.6) * sky * 0.6;

    outColor = vec4(albedo.rgb * light, albedo.a);
}
