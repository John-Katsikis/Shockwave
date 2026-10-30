// Local reflection probes (G07): Renderer/ReflectionProbes.h in GLSL. Each draw names up to
// two probes (DrawData::reflections, chosen on the CPU by its bounds); each pixel box-projects
// its reflection ray into them, weights them by how deep inside their boxes it is, and blends
// them over the global environment's reflection. Include after frame.glsl and the
// declaration of linearSampler.

// Captured cubes, a table of their own beside the 2D one (RendererPipelines.cpp createBindless).
layout(set = 0, binding = 7) uniform textureCube reflectionCubes[];

// Must match ReflectionProbeData in RendererImpl.h.
struct ReflectionProbeData {
    mat4 worldToBox;   // rigid
    vec3 halfExtents;   // metres
    float blendDistance;
    vec3 capturePosition;   // world space
    uint cube;   // slot in reflectionCubes; 0xFFFFFFFF: not loaded, the probe is skipped
    int priority;
    uint pad0, pad1, pad2;
};
// Every captured cube's levels: roughness 0, 0.2 .. 1 (kReflectionLevels), like specularCube's.
const float kReflectionLevels = 6.0;

layout(buffer_reference, scalar) readonly buffer ReflectionProbes {
    ReflectionProbeData probes[];
};

// reflectionBoxProject: false (and the probe left out) outside the box, for a zero ray, or
// without an exit ahead. Never divides by a zero component.
bool reflectionBoxProject(ReflectionProbeData probe, vec3 point, vec3 R, out vec3 lookup) {
    vec3 p = (probe.worldToBox * vec4(point, 1.0)).xyz;
    vec3 r = mat3(probe.worldToBox) * R;
    vec3 h = probe.halfExtents;
    lookup = R;
    if (any(greaterThan(abs(p), h))) return false;
    float exitAt = 3.4e38;
    for (int i = 0; i < 3; ++i) {
        if (abs(r[i]) < 1e-8) continue;
        exitAt = min(exitAt, ((r[i] > 0.0 ? h[i] : -h[i]) - p[i]) / r[i]);
    }
    if (!(exitAt >= 0.0) || exitAt >= 3.4e38) return false;
    vec3 d = point + exitAt * R - probe.capturePosition;
    lookup = dot(d, d) > 1e-12 ? d : R;
    return true;
}

// reflectionProbeWeight: 1 deeper than blendDistance inside, 0 at the faces and outside.
float reflectionProbeWeight(ReflectionProbeData probe, vec3 point) {
    vec3 inside = probe.halfExtents - abs((probe.worldToBox * vec4(point, 1.0)).xyz);
    float depth = min(inside.x, min(inside.y, inside.z));
    if (!(depth >= 0.0)) return 0.0;
    if (!(probe.blendDistance > 0.0)) return 1.0;
    return clamp(depth / probe.blendDistance, 0.0, 1.0);
}

// The shares of the draw's probes at this pixel (blendReflectionProbes), and their box-
// projected lookups. picked: probe index + 1 in the low and high 16 bits; 0: none.
vec2 reflectionShares(FrameBuffer frame, uint picked, vec3 point, vec3 R, out vec3 lookups[2], out uint slots[2]) {
    ReflectionProbes table = ReflectionProbes(frame.reflectionProbes);
    uint count = (picked >> 16) != 0u ? 2u : 1u;
    vec2 w = vec2(0.0);
    int priorities[2] = int[](0, 0);
    for (uint i = 0u; i < count; ++i) {
        uint index = ((i == 0u ? picked : picked >> 16) & 0xFFFFu) - 1u;
        slots[i] = 0xFFFFFFFFu;
        lookups[i] = R;
        if (index >= frame.reflectionProbeCount) continue;
        ReflectionProbeData probe = table.probes[index];
        priorities[i] = probe.priority;
        if (probe.cube == 0xFFFFFFFFu) continue;
        if (!reflectionBoxProject(probe, point, R, lookups[i])) continue;
        w[i] = reflectionProbeWeight(probe, point);
        slots[i] = probe.cube;
    }
    if (count == 1u) return vec2(w.x, 0.0);
    if (priorities[0] == priorities[1]) {
        float sum = w.x + w.y;
        return sum > 0.0 ? w * (min(sum, 1.0) / sum) : vec2(0.0);
    }
    return vec2(w.x, w.y * (1.0 - w.x));
}

// The specular source for the split sum: the global environment's prefiltered reflection
// (global, already read at this roughness) with the local probes blended over it, in
// linear HDR, read at the same roughness-to-level mapping. shares comes back for the
// debug view.
vec3 localReflections(FrameBuffer frame, uint picked, vec3 point, vec3 R, float roughness, vec3 global, out vec2 shares) {
    vec3 lookups[2];
    uint slots[2];
    shares = reflectionShares(frame, picked, point, R, lookups, slots);
    float coverage = min(shares.x + shares.y, 1.0);
    if (coverage <= 0.0) return global;
    vec3 local = vec3(0.0);
    for (int i = 0; i < 2; ++i) {
        if (shares[i] <= 0.0) continue;
        float lod = roughness * (kReflectionLevels - 1.0);
        local += shares[i] * textureLod(samplerCube(reflectionCubes[nonuniformEXT(slots[i])], linearSampler), lookups[i], lod).rgb;
    }
    return local + (1.0 - coverage) * global;
}
