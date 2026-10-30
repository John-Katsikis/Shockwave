// Shared by every pass that draws the scene's meshes (mesh, shadow): the draw table, the
// vertex layout, and how a material slot finds its texel. One copy, so the alpha a shadow
// map cuts out is exactly the alpha the scene cuts out (G02).
//
// Needs GL_EXT_buffer_reference, GL_EXT_buffer_reference_uvec2, GL_EXT_scalar_block_layout
// and GL_EXT_nonuniform_qualifier.

struct Vertex {
    vec3 position;
    vec3 normal;
    vec2 uv;
    vec4 color;
    vec4 tangent;   // xyz along +u; w: handedness of the bitangent
};

layout(buffer_reference, scalar) readonly buffer VertexBuffer {
    Vertex vertices[];
};
layout(buffer_reference, scalar) readonly buffer Uv1Buffer {
    vec2 uv1[];
};
layout(buffer_reference, scalar) readonly buffer LightmapUvBuffer {
    vec2 lightmapUv[];
};
// A draw's probe sample (G06): irradiance / pi spherical harmonics, rgb in xyz, in the
// order of Assets/LightingData.h. Must match ProbeData in RendererImpl.h.
struct ProbeData {
    vec4 sh[9];
};
layout(buffer_reference, scalar) readonly buffer ProbeBuffer {
    ProbeData probe;
};
// DrawData::indirectSource: where diffuse light from the surroundings comes from.
const uint kIndirectEnvironment = 0u, kIndirectLightmap = 1u, kIndirectProbes = 2u;

// How one material slot reads its texture. Must match SlotBinding in RendererImpl.h.
struct SlotBinding {
    vec2 offset;
    vec2 scale;
    float rotation;   // radians; KHR_texture_transform's
    uint samplerAndSet;   // bits 0..15: sampler, bit 16: UV set 1
};

// Slots, in MaterialSlot order.
const uint kBaseColor = 0u, kNormal = 1u, kMetallicRoughness = 2u, kEmissive = 3u, kOcclusion = 4u;
// DrawData::flags
const uint kDerivativeTangents = 1u;   // the normal map's UVs don't follow the vertex tangents

// A draw's detail layer and second layer (A02). Must match LayerData in RendererImpl.h.
struct LayerData {
    uint detailColor;   // bindless slots; 0: the white texture (detailNormal: none)
    uint detailNormal;
    uint detailMask;
    uint detailMaskChannel;
    float detailStrength;
    float detailNormalScale;
    float detailRoughness;
    float detailRoughnessStrength;
    vec2 detailFade;   // metres from the camera: starts, finishes; finishes 0: never fades
    uint layerMask;
    uint layerBaseColor;
    uint layerNormal;
    uint layerMetallicRoughness;
    uint layerMaskSource;   // 0..3 the mask texture's channel, 4..6 the vertex colour's
    uint flags;
    vec4 layerBaseColorFactor;
    float layerMetallic;
    float layerRoughness;
    float layerNormalScale;
    float pad;
    SlotBinding slots[7];   // MaterialSlot::DetailColor onwards
};
layout(buffer_reference, scalar) readonly buffer LayerBuffer {
    LayerData layer;
};
// LayerData::flags
const uint kLayerDetail = 1u, kLayerSecond = 2u;
const uint kLayerDetailDerivativeTangents = 4u, kLayerSecondDerivativeTangents = 8u;
// LayerData::slots
const uint kDetailColor = 0u, kDetailNormal = 1u, kDetailMask = 2u, kLayerMaskSlot = 3u;
const uint kLayerBaseColor = 4u, kLayerNormal = 5u, kLayerMetallicRoughness = 6u;

// One entry per draw in the frame's draw table. Must match DrawData in RendererImpl.h.
struct DrawData {
    mat4 mvp;
    mat4 model;
    mat4 normalMatrix;
    vec4 tint;
    vec2 uvScale;
    uvec2 vertexBuffer;   // a VertexBuffer: MoltenVK (macOS) reads references kept inside buffers as null
    uint textureIndex;
    float alphaCutoff;
    float roughness;
    float metallic;
    vec3 emissive;
    uint normalTexture;   // 0: none
    uint metallicRoughnessTexture;
    uint emissiveTexture;
    uint occlusionTexture;
    float normalScale;
    float occlusionStrength;
    uint flags;
    uvec2 uv1Buffer;   // a Uv1Buffer; 0: the mesh has no second UV set, UV0 stands in
    SlotBinding slots[5];
    uvec2 layers;   // a LayerBuffer; 0: no detail or second layer
    uvec2 lightmapUvBuffer;   // a LightmapUvBuffer, with kIndirectLightmap
    uint lightmapTexture;   // bindless slot, with kIndirectLightmap
    uint indirectSource;
    uvec2 probes;   // a ProbeBuffer, with kIndirectProbes
};

layout(buffer_reference, scalar) readonly buffer DrawBuffer {
    DrawData draws[];
};

// Both UV sets of a vertex, with the draw's uvScale (MeshRenderer tiling) applied.
vec4 vertexUvs(DrawData draw, Vertex v, uint index) {
    vec2 uv1 = v.uv;
    if (draw.uv1Buffer != uvec2(0u)) uv1 = Uv1Buffer(draw.uv1Buffer).uv1[index];
    return vec4(v.uv, uv1) * draw.uvScale.xyxy;
}

// The slot's UVs: uv' = offset + R(rotation) * (scale * uv), R = [cos sin; -sin cos].
// An identity transform returns uv untouched, bit for bit.
vec2 slotUv(SlotBinding slot, vec4 uvs) {
    vec2 uv = (slot.samplerAndSet & 0x10000u) != 0u ? uvs.zw : uvs.xy;
    uv *= slot.scale;
    if (slot.rotation != 0.0) {
        float c = cos(slot.rotation), s = sin(slot.rotation);
        uv = vec2(c * uv.x + s * uv.y, -s * uv.x + c * uv.y);
    }
    return uv + slot.offset;
}
uint slotSampler(SlotBinding slot) { return slot.samplerAndSet & 0xFFFFu; }

#ifdef DRAW_SAMPLING
// The bindless tables, set 0 (RendererPipelines.cpp createBindless).
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 6) uniform sampler materialSamplers[];

vec4 sampleSlot(uint image, SlotBinding slot, vec2 uv) {
    return texture(sampler2D(textures[nonuniformEXT(image)], materialSamplers[nonuniformEXT(slotSampler(slot))]), uv);
}
// Base color alpha, as alpha testing sees it in every pass.
float baseAlpha(DrawBuffer table, uint draw, vec4 uvs, float vertexAlpha) {
    SlotBinding slot = table.draws[draw].slots[kBaseColor];
    return sampleSlot(table.draws[draw].textureIndex, slot, slotUv(slot, uvs)).a * vertexAlpha;
}
#endif
