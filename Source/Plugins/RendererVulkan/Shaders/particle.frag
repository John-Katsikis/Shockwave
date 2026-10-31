#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_GOOGLE_include_directive : require

// Particles: texture times vertex color times the batch's tint, in linear HDR. Blending
// (alpha, add, subtract, multiply) is fixed-function, one pipeline per mode. The reactive
// variant (G09) writes how much each particle changes what's behind it instead, for TAA.
//
// A04: soft particles fade against the scene's depth; distortion batches show the scene
// behind them, displaced; lit batches can take every light in the frame (scene lighting).
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler linearSampler;

#include "frame.glsl"
#include "clusters.glsl"
#include "fog.glsl"

// Must match ParticleBatchData in RendererImpl.h.
struct ParticleBatchData {
    vec4 tint;          // linear; rgb may exceed 1
    vec4 edgeColor;     // alpha cutoff edge: rgb, a = width
    vec3 lightDirection;
    uint texture;
    vec3 lightColor;
    uint alphaTexture;
    vec3 ambient;
    uint flags;
    float emissive;
    uint blend;         // ParticleBlend
    float softDistance; // metres; 0: no fade against the scene
    float softNear;     // metres; 0: no fade near the camera
    float softNearOffset;
    float distortion;   // metres at full strength
    float pad0, pad1;
};

layout(buffer_reference, scalar) readonly buffer ParticleVertices {
    uint unused[];
};
layout(buffer_reference, scalar) readonly buffer ParticleBatches {
    ParticleBatchData batches[];
};

// Must match ParticlePushConstants in RendererParticles.cpp.
layout(push_constant, scalar) uniform PushConstants {
    mat4 viewProjection;
    ParticleVertices vertices;
    ParticleBatches batches;
    FrameBuffer frame;
    uint depthSlot;        // scene depth (reverse-Z) to fade and reject against; 0: none
    uint backgroundSlot;   // the scene before particles, for distortion; 0: none
    uint flags;            // kPass*
    uint pad;
} pc;

// Flags, as in Renderer/Particles.h.
const uint kLit = 1u << 0;
const uint kFlipbookBlend = 1u << 1;
const uint kAlphaTexture = 1u << 2;
const uint kClampUv = 1u << 3;
const uint kAlphaCutoff = 1u << 4;
const uint kDistortion = 1u << 5;
const uint kBlendMultiply = 4u;
// ParticlePushConstants::flags, as in RendererParticles.cpp.
const uint kPassSceneLighting = 1u << 0;
const uint kPassSoft = 1u << 1;
// The furthest distortion moves a look-up, in uv: a tenth of the screen.
const float kMaxDistortionUv = 0.1;

layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec2 inAlphaUv;
layout(location = 3) in vec2 inNextUv;
layout(location = 4) in vec3 inNormal;
layout(location = 5) in vec2 inParams;
layout(location = 6) flat in uint inBatch;
layout(location = 7) in vec3 inWorldPosition;
layout(location = 8) in vec4 inClipTangent;
layout(location = 9) in vec4 inClipBinormal;
layout(location = 0) out vec4 outColor;

layout(constant_id = 0) const bool kReactive = false;
const uint kBlendOpaque = 0u, kBlendAlpha = 1u;

vec4 sampleTexture(uint index, vec2 uv, bool clampUv) {
    if (clampUv) uv = clamp(uv, 0.0, 1.0);
    return texture(sampler2D(textures[nonuniformEXT(index)], linearSampler), uv);
}

vec3 linearToSrgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

// Positive view distance of a reverse-Z device depth at this pixel; the sky (0) is infinitely far.
float viewDistance(float depth) {
    if (depth <= 0.0) return 1e30;
    vec2 ndc = vec2(gl_FragCoord.x / pc.frame.renderSize.x * 2.0 - 1.0, 1.0 - gl_FragCoord.y / pc.frame.renderSize.y * 2.0);
    vec4 h = pc.frame.inverseProjection * vec4(ndc, depth, 1.0);
    return -h.z / h.w;
}

float sceneDepth(ivec2 pixel) {
    ivec2 size = ivec2(pc.frame.renderSize);
    return texelFetch(sampler2D(textures[pc.depthSlot], linearSampler), clamp(pixel, ivec2(0), size - 1), 0).r;
}

// Renderer/Particles.h softParticleFade: Effekseer's soft particle fade.
float softFade(ParticleBatchData b, float zp) {
    float zs = viewDistance(sceneDepth(ivec2(gl_FragCoord.xy)));
    float fade = 1.0;
    if (b.softDistance > 0.0) fade = min(fade, (zs - zp) / b.softDistance);
    if (b.softNear > 0.0) fade = min(fade, (zp - b.softNearOffset) / b.softNear);
    return clamp(fade, 0.0, 1.0);
}

// As mesh.frag's: a light's range window over inverse-square falloff.
float rangeFalloff(float distance, float range) {
    float ratio = distance / range;
    float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    return window * window / (distance * distance + 1.0);
}

// One light's diffuse light on a particle: Lambert, without shadows.
vec3 particleLight(uint i, vec3 n) {
    LightData l = pc.frame.lights[i];
    vec3 toLight;
    float attenuation = 1.0;
    if (l.type == 0u) {
        toLight = -l.direction;
    } else {
        vec3 offset = l.position - inWorldPosition;
        float distance = length(offset);
        if (distance >= l.range) return vec3(0.0);
        toLight = offset / max(distance, 1e-4);
        attenuation = rangeFalloff(distance, l.range);
        if (l.type == 2u) attenuation *= smoothstep(l.cosOuter, l.cosInner, dot(-toLight, l.direction));
    }
    return l.color * (max(dot(n, toLight), 0.0) * attenuation);
}

// particle_lighting scene: every directional light, the point and spot lights of this
// pixel's cluster (every one without lists), and the sky and ground's ambient light.
vec3 sceneLight(vec3 n, float z) {
    FrameBuffer frame = pc.frame;
    vec3 light = vec3(0.0);
    for (uint i = 0u; i < frame.directionalCount; ++i) light += particleLight(i, n);
    uint list = clusterListStart(frame, gl_FragCoord.xy, z);
    ClusterLists lists = ClusterLists(frame.clusters);
    uint listed = list != kNoCluster ? lists.words[list] : kClusterOverflow;
    if ((listed & kClusterOverflow) != 0u) {
        for (uint i = frame.directionalCount; i < frame.lightCount; ++i) light += particleLight(i, n);
    } else {
        for (uint k = 0u; k < listed; ++k)
            light += particleLight((lists.words[list + 1u + (k >> 2)] >> ((k & 3u) * 8u)) & 0xFFu, n);
    }
    return light + mix(frame.groundAmbient, frame.skyAmbient, n.y * 0.5 + 0.5);
}

void main() {
    ParticleBatchData b = pc.batches.batches[inBatch];
    bool clampUv = (b.flags & kClampUv) != 0u;
    vec4 tex = sampleTexture(b.texture, inUv, clampUv);
    if ((b.flags & kFlipbookBlend) != 0u) tex = mix(tex, sampleTexture(b.texture, inNextUv, clampUv), inParams.x);
    if ((b.flags & kAlphaTexture) != 0u) {
        vec4 a = sampleTexture(b.alphaTexture, inAlphaUv, clampUv);
        tex.a *= a.r * a.a;
    }
    float zp = -(pc.frame.view * vec4(inWorldPosition, 1.0)).z;
    vec4 color;
    if ((b.flags & kDistortion) != 0u) {
        // Effekseer's background distortion: the texture's red and green, -1..1, times the
        // vertex colour's (as authored, sRGB), move the look-up up to b.distortion metres
        // along the tangent and binormal, as they land on screen.
        if (!kReactive && pc.backgroundSlot == 0u) discard;
        vec2 scale = (tex.rg * 2.0 - 1.0) * linearToSrgb(inColor.rgb).rg * b.distortion;
        vec2 size = pc.frame.renderSize;
        vec2 uv = gl_FragCoord.xy / size;
        vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
        vec2 moved = ndc + (inClipTangent.xy / inClipTangent.w - ndc) * scale.x +
                     (inClipBinormal.xy / inClipBinormal.w - ndc) * scale.y;
        vec2 offset = vec2(moved.x - ndc.x, ndc.y - moved.y) * 0.5;   // uv runs down
        float length2 = dot(offset, offset);
        if (length2 > kMaxDistortionUv * kMaxDistortionUv) offset *= kMaxDistortionUv * inversesqrt(length2);
        vec2 displaced = uv + offset;
        // Nothing nearer than the particle moves (no foreground bleeding into the haze), and
        // nothing comes from off the image.
        if (any(lessThan(displaced, vec2(0.0))) || any(greaterThan(displaced, vec2(1.0))) ||
            (pc.depthSlot != 0u && viewDistance(sceneDepth(ivec2(displaced * size))) < zp))
            displaced = uv;
        vec3 background = kReactive ? vec3(0.0) : texture(sampler2D(textures[pc.backgroundSlot], linearSampler), displaced).rgb;
        color = vec4(background, tex.a * inColor.a * b.tint.a);
    } else {
        color = tex * inColor * b.tint;
        color.rgb *= b.emissive;
    }

    if ((b.flags & kAlphaCutoff) != 0u) {
        float threshold = inParams.y;
        if (color.a <= max(threshold, 0.0)) discard;
        // Pixels just above the threshold take the edge color: burning, dissolving edges.
        if (color.a - threshold < b.edgeColor.a && (b.flags & kDistortion) == 0u) color.rgb = b.edgeColor.rgb;
    }
    if ((b.flags & (kLit | kDistortion)) == kLit) {
        // The side facing the camera is the lit side, whichever way the triangle winds
        // (billboards face the camera; a model's inside shows its back faces).
        vec3 n = normalize(inNormal);
        if (dot(n, pc.frame.cameraPosition - inWorldPosition) < 0.0) n = -n;
        if ((pc.flags & kPassSceneLighting) != 0u) color.rgb *= sceneLight(n, zp);
        else color.rgb *= max(dot(n, -b.lightDirection), 0.0) * b.lightColor + b.ambient;
    }
    // Soft particles: fading alpha fades every blend mode, as each scales by alpha (see
    // below for multiply); opaque batches don't blend, so don't fade.
    if ((pc.flags & kPassSoft) != 0u && pc.depthSlot != 0u && b.blend != kBlendOpaque) color.a *= softFade(b, zp);
    // Multiply blends dst * src; fade toward white (no change) as alpha drops.
    if (b.blend == kBlendMultiply) color.rgb = mix(vec3(1.0), color.rgb, color.a);
    // Fog (A06) between the camera and the particle: what it shows becomes S + T L over the
    // fogged scene; light it adds or takes away only dims (T E, no second S); a multiply
    // fades toward white. Distortion shows the already fogged scene, so takes none.
    if (!kReactive && pc.frame.fogMode != kFogNone && pc.frame.debugView == 0u && (b.flags & kDistortion) == 0u) {
        vec4 fog = sceneFog(pc.frame, gl_FragCoord.xy, inWorldPosition);
        if (b.blend == kBlendOpaque || b.blend == kBlendAlpha) color.rgb = fog.rgb + fog.a * color.rgb;
        else if (b.blend == kBlendMultiply) color.rgb = mix(vec3(1.0), color.rgb, fog.a);
        else color.rgb *= fog.a;
    }
    if (kReactive) {
        // Opaque particles have no motion vectors, so nothing of history is kept under them;
        // alpha blending replaces alpha of what's behind (distortion too: it changes what's
        // behind wherever it covers); adding and subtracting change it by up to the
        // brightest channel; multiplying by how far from white it is.
        float reactive = 1.0;
        if (b.blend == kBlendAlpha || (b.flags & kDistortion) != 0u) reactive = color.a;
        else if (b.blend == kBlendMultiply) reactive = 1.0 - min(color.r, min(color.g, color.b));
        else if (b.blend != kBlendOpaque) reactive = max(color.r, max(color.g, color.b)) * color.a;
        outColor = vec4(clamp(reactive, 0.0, 1.0));
        return;
    }
    outColor = color;
}
