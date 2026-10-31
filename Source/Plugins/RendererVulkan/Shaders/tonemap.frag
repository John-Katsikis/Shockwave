#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

// HDR scene in, display colors out: bloom, white balance, exposure (manual, or metered by
// exposure_adapt.comp), a tonemapping curve, then creative LUTs on the display-linear result
// (A05, Renderer/PostProcess.h). The target is an sRGB image, so this writes linear values
// and the hardware encodes them, once.
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler linearSampler;

// Must match TonemapPushConstants in Renderer.cpp.
layout(push_constant, scalar) uniform PushConstants {
    vec2 outputSize;
    uint sceneTexture;
    float exposureScale;
    uint tonemapper;   // 0 ACES, 1 AgX, 2 Reinhard, 3 none; 99: debug view, pass through
    uint bloomTexture;   // the bloom chain's top level (RendererBloom.cpp)
    float bloomIntensity;   // 0: none
    float autoExposure;   // 0 manual .. 1 metered, blended in stops
    mat3 whiteBalance;   // linear sRGB, Bradford to D65 (identity: none)
    uint lutTexture[2];   // LUT atlases (Renderer/PostProcess.h lutTexture); 0: none
    uint lutSize[2];   // entries a side; top bit: the table is on linear values, not sRGB-encoded
    float lutWeight[2];
    uint pad0;
    uvec2 exposure;   // ExposureState (exposure.glsl)
    float manualEv;   // stops
} pc;

layout(buffer_reference, scalar) readonly buffer ExposureState {
    uint histogram[128];
    float ev;
} ;

float srgbEncode(float c) {
    c = clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
float srgbDecode(float c) {
    c = clamp(c, 0.0, 1.0);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

// One table, trilinear: two bilinear taps in the atlas's blue slices. Must match sampleLut
// in Renderer/Private/PostProcess.cpp.
vec3 sampleLut(uint slot, uint sizeAndFlags, vec3 c) {
    bool linear = (sizeAndFlags & 0x80000000u) != 0u;
    float n = float(sizeAndFlags & 0xFFFFu);
    if (!linear) c = vec3(srgbEncode(c.r), srgbEncode(c.g), srgbEncode(c.b));
    vec3 p = clamp(c, 0.0, 1.0) * (n - 1.0);
    float b0 = min(floor(p.z), n - 1.0);
    float b1 = min(b0 + 1.0, n - 1.0);
    vec2 atlas = vec2(n * n, n);
    vec2 uv0 = vec2(p.x + 0.5 + b0 * n, p.y + 0.5) / atlas;
    vec2 uv1 = vec2(p.x + 0.5 + b1 * n, p.y + 0.5) / atlas;
    vec3 out0 = textureLod(sampler2D(textures[nonuniformEXT(slot)], linearSampler), uv0, 0.0).rgb;
    vec3 out1 = textureLod(sampler2D(textures[nonuniformEXT(slot)], linearSampler), uv1, 0.0).rgb;
    vec3 v = mix(out0, out1, p.z - b0);
    return linear ? v : vec3(srgbDecode(v.r), srgbDecode(v.g), srgbDecode(v.b));
}

layout(location = 0) out vec4 outColor;

// ACES filmic fit (Stephen Hill), with the input and output color transforms.
vec3 aces(vec3 color) {
    const mat3 inputMatrix = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
    const mat3 outputMatrix = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);
    vec3 v = inputMatrix * color;
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return outputMatrix * (a / b);
}

// AgX base look (Troy Sobotka), polynomial fit of the default contrast curve.
vec3 agx(vec3 color) {
    const mat3 inset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051, 0.0784335999999992,
                            0.878468636469772, 0.0784336, 0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438, -0.0980208811401368,
                             1.15190312990417, -0.0980434501171241, -0.0990297440797205, -0.0989611768448433,
                             1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    vec3 v = inset * max(color, 1e-10);
    v = clamp((log2(v) - minEv) / (maxEv - minEv), 0.0, 1.0);
    vec3 v2 = v * v, v4 = v2 * v2;
    v = 15.5 * v4 * v2 - 40.14 * v4 * v + 31.96 * v4 - 6.868 * v2 * v + 0.4298 * v2 + 0.1191 * v - 0.00232;
    v = outset * v;
    return pow(max(v, 0.0), vec3(2.2));   // the curve outputs display-encoded values
}

vec3 reinhard(vec3 color) {
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return color / (1.0 + luminance);
}

void main() {
    vec2 size = vec2(textureSize(sampler2D(textures[nonuniformEXT(pc.sceneTexture)], linearSampler), 0));
    // Stay half a texel inside the edges: the shared sampler repeats.
    vec2 uv = clamp(gl_FragCoord.xy / pc.outputSize, 0.5 / size, 1.0 - 0.5 / size);
    vec3 hdr = texture(sampler2D(textures[nonuniformEXT(pc.sceneTexture)], linearSampler), uv).rgb;
    if (pc.bloomIntensity > 0.0) {
        // A mix rather than an add: bloom moves light around instead of adding more of it.
        vec2 bloomSize = vec2(textureSize(sampler2D(textures[nonuniformEXT(pc.bloomTexture)], linearSampler), 0));
        vec2 bloomUv = clamp(gl_FragCoord.xy / pc.outputSize, 0.5 / bloomSize, 1.0 - 0.5 / bloomSize);
        vec3 bloom = texture(sampler2D(textures[nonuniformEXT(pc.bloomTexture)], linearSampler), bloomUv).rgb;
        hdr = mix(hdr, bloom, pc.bloomIntensity);
    }
    vec3 color;
    if (pc.tonemapper == 99u) {
        color = hdr;
    } else {
        hdr = pc.whiteBalance * hdr;
        float scale = pc.exposureScale;
        if (pc.autoExposure > 0.0 && pc.exposure != uvec2(0u))
            scale = exp2(mix(pc.manualEv, ExposureState(pc.exposure).ev, pc.autoExposure));
        hdr *= scale;
        if (pc.tonemapper == 0u) color = aces(hdr);
        else if (pc.tonemapper == 1u) color = agx(hdr);
        else if (pc.tonemapper == 2u) color = reinhard(hdr);
        else color = hdr;
        color = clamp(color, 0.0, 1.0);
        // Creative LUTs on display-linear colour: each takes its share of the result.
        vec3 graded = color;
        for (int i = 0; i < 2; ++i)
            if (pc.lutTexture[i] != 0u && pc.lutWeight[i] > 0.0)
                graded += (sampleLut(pc.lutTexture[i], pc.lutSize[i], color) - color) * pc.lutWeight[i];
        color = graded;
    }
    outColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
