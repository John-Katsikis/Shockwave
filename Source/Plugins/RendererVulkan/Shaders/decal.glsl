// Projected decals (G08): the GLSL twin of Renderer/Decals.h's per-pixel rules, and the
// frame's decal table. Must match DecalData in RendererImpl.h.
struct DecalData {
    mat4 boxToWorld;   // the unit box [-0.5, 0.5]^3 to the world
    mat4 worldToBox;
    vec4 color;   // linear rgb, a: opacity
    vec3 facing;   // world direction surfaces face to take it in full (the box's +Z)
    float fadeStartCos;   // cos(angle_fade): full strength up to here, nothing side-on
    uint texture;   // bindless slot
    uint channels;   // 1: colour, 2: roughness and metallic
    uint receiverMask;
    float roughness;
    float metallic;
    float pad0, pad1, pad2;
};
layout(buffer_reference, scalar) readonly buffer DecalBuffer {
    DecalData decals[];
};
const uint kDecalAlbedo = 1u, kDecalRoughnessMetallic = 2u;

float decalAngleFade(DecalData d, vec3 normal) {
    float c = dot(normal, d.facing);
    if (!(c > 0.0)) return 0.0;
    return clamp(c / d.fadeStartCos, 0.0, 1.0);
}
