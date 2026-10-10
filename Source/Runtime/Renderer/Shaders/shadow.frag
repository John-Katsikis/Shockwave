#version 460
#extension GL_EXT_nonuniform_qualifier : require

// Only alpha-tested surfaces (leaves, fences) need anything here: their cut-out parts
// mustn't cast. Everything else just writes depth.
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler linearSampler;

layout(location = 0) in vec2 vUv;
layout(location = 1) in float vAlpha;
layout(location = 2) flat in uint vTexture;
layout(location = 3) flat in float vAlphaCutoff;

void main() {
    if (vAlphaCutoff >= 0.0) {
        float alpha = texture(sampler2D(textures[nonuniformEXT(vTexture)], linearSampler), vUv).a * vAlpha;
        if (alpha < vAlphaCutoff) discard;
    }
}
