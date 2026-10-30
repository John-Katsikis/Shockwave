#pragma once

// Renderer's private state, shared by the Renderer*.cpp files. Not part of the module API.

#include "VulkanRenderer.h"

#include <Core/Jobs.h>
#include <Platform/Window.h>
#include <RHI/Context.h>
#include <RHI/Pipeline.h>
#include <RHI/RenderGraph.h>
#include <RHI/Swapchain.h>

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace shockwave {

namespace detail {

constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kMaxMeshes = 65536;
// Scene color, BRDF table, two for bloom: a new bloom chain takes the
// slot the old one isn't using, so it can be made while frames still read the old one.
// Shadow atlases likewise have two pairs of slots, and ambient occlusion two slots.
constexpr uint32_t kInternalTextureSlots = 10;
// Spare bindless slots for textures being replaced (reloads, texture_quality): the new image
// goes into a spare while frames in flight still read the old slot, which becomes a spare
// once they finish. Below the internal slots.
constexpr uint32_t kSpareTextureSlots = 256;
// Material samplers (G02): two banks of kMaxSamplers, so changing anisotropy writes the
// bank no frame in flight is reading.
constexpr uint32_t kSamplerBanks = 2;
// Image-based lighting's cube maps (environment, specular, irradiance) sit beside the
// bindless table in the same set, so they come out of the same descriptor budget.
constexpr uint32_t kEnvironmentCubes = 3;
// The scene renders in HDR; tonemapping to the display happens at the end of the frame.
constexpr VkFormat kSceneColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
// The tonemapped result at output resolution: what the window or editor viewport shows.
constexpr VkFormat kDisplayFormat = VK_FORMAT_R8G8B8A8_SRGB;

// How one material slot reads its texture. Must match SlotBinding in shaders/draw.glsl.
struct SlotBinding {
    glm::vec2 offset{0.0f};
    glm::vec2 scale{1.0f};
    float rotation = 0.0f;
    uint32_t samplerAndSet = 0;   // bits 0..15: the sampler's bindless slot, bit 16: UV set 1
};
static_assert(sizeof(SlotBinding) == 24, "SlotBinding must match the shader's scalar layout");
constexpr uint32_t kSlotUvSet1 = 0x10000u;
constexpr uint32_t kDrawDerivativeTangents = 1u;   // DrawData::flags; see shaders/mesh.frag

// One entry per draw in the frame's draw table. Must match DrawData in shaders/draw.glsl.
struct DrawData {
    glm::mat4 mvp;
    glm::mat4 model;
    glm::mat4 normalMatrix;
    glm::vec4 tint;
    glm::vec2 uvScale;
    VkDeviceAddress vertexBuffer;
    uint32_t textureIndex;
    float alphaCutoff;
    float roughness;
    float metallic;
    glm::vec3 emissive;
    uint32_t normalTexture;   // 0: none
    uint32_t metallicRoughnessTexture;
    uint32_t emissiveTexture;
    uint32_t occlusionTexture;
    float normalScale;
    float occlusionStrength;
    uint32_t flags;
    VkDeviceAddress uv1Buffer;   // the mesh's second UV set; 0: none
    SlotBinding slots[kMaterialSlots];
    VkDeviceAddress layers;   // this draw's LayerData; 0: no detail or second layer
    // Baked lighting (G05, G06): where the draw's diffuse light from its surroundings comes from.
    VkDeviceAddress lightmapUvBuffer;   // the mesh's lightmap UVs; 0: none
    uint32_t lightmapTexture;   // bindless slot, with kIndirectLightmap
    uint32_t indirectSource;   // kIndirectEnvironment, kIndirectLightmap or kIndirectProbes
    VkDeviceAddress probes;   // this draw's ProbeData, with kIndirectProbes
    // Local reflection probes (G07): index + 1 in the frame's table of the first (low 16
    // bits) and second (high 16); 0: the global environment only.
    uint32_t reflections;
    uint32_t pad;
};
static_assert(
    offsetof(DrawData, vertexBuffer) == 216 && offsetof(DrawData, emissive) == 240 &&
        offsetof(DrawData, normalTexture) == 252 && offsetof(DrawData, occlusionStrength) == 272 &&
        offsetof(DrawData, uv1Buffer) == 280 && offsetof(DrawData, slots) == 288 && offsetof(DrawData, layers) == 408 &&
        offsetof(DrawData, lightmapUvBuffer) == 416 && offsetof(DrawData, indirectSource) == 428 &&
        offsetof(DrawData, probes) == 432 && offsetof(DrawData, reflections) == 440 && sizeof(DrawData) == 448,
    "DrawData must match the shader's scalar layout");
constexpr uint32_t kIndirectEnvironment = 0u, kIndirectLightmap = 1u,
                   kIndirectProbes = 2u;   // DrawData::indirectSource
// A draw's probe sample: irradiance / pi spherical harmonics (Assets/LightingData.h order),
// in a table of its own like LayerData. Must match ProbeData in shaders/draw.glsl.
struct ProbeData {
    glm::vec4 sh[9];   // rgb; w unused
};
static_assert(sizeof(ProbeData) == 144, "ProbeData must match the shader's scalar layout");

// A draw's detail layer and second layer (A02), in a table of their own so draws without
// them don't carry the weight. Must match LayerData in shaders/draw.glsl.
constexpr uint32_t kLayerDetail = 1u, kLayerSecond = 2u;   // LayerData::flags
constexpr uint32_t kLayerDetailDerivativeTangents = 4u, kLayerSecondDerivativeTangents = 8u;
struct LayerData {
    uint32_t detailColor, detailNormal, detailMask;   // bindless slots; 0: white (detailNormal: none)
    uint32_t detailMaskChannel;
    float detailStrength, detailNormalScale, detailRoughness, detailRoughnessStrength;
    glm::vec2 detailFade;
    uint32_t layerMask, layerBaseColor, layerNormal, layerMetallicRoughness;
    uint32_t layerMaskSource;   // 0..3 the mask texture's channel, 4..6 the vertex colour's
    uint32_t flags;
    glm::vec4 layerBaseColorFactor;
    float layerMetallic, layerRoughness, layerNormalScale, pad;
    SlotBinding slots[kLayerSlots];
};
static_assert(
    offsetof(LayerData, detailFade) == 32 && offsetof(LayerData, layerBaseColorFactor) == 64 &&
        offsetof(LayerData, slots) == 96 && sizeof(LayerData) == 264,
    "LayerData must match the shader's scalar layout");
constexpr uint32_t kNoLayers = 0xFFFFFFFFu;

// Shadow maps hold reverse-Z depth, like the scene: cleared to 0, closer is greater.
constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;
constexpr uint32_t kMaxCascades = 4;
constexpr uint32_t kMaxLocalShadowViews = 64;   // tiles in the spot/point light atlas
constexpr uint32_t kMaxShadowViews = kMaxCascades + kMaxLocalShadowViews;
constexpr uint32_t kNoShadow = 0xFFFFFFFFu;
constexpr uint32_t kNoSkin = 0xFFFFFFFFu;

// Must match LightData and FrameBuffer in shaders/frame.glsl.
struct LightData {
    glm::vec3 position;
    uint32_t type;
    glm::vec3 direction;
    float range;
    glm::vec3 color;
    float cosOuter;
    float cosInner;
    uint32_t shadowView;   // first ShadowView (point lights: six, one per cube face); kNoShadow: none
    float shadowStrength;
    float shadowSoftness;   // filter radius in shadow map texels
    float shadowBias;   // normal offset, in shadow map texels
    float pad[3];
};
static_assert(sizeof(LightData) == 80, "LightData must match the shader's scalar layout");
// One shadow map: a sun cascade, a spot light, or one face of a point light.
struct ShadowView {
    glm::mat4 viewProjection;
    glm::vec4 rect;   // where it is in its atlas: uv offset (xy) and scale (zw)
    float texelSize;   // world size of a texel: at distance 1 for perspective views
    uint32_t perspective;
    float pad[2];
};
static_assert(sizeof(ShadowView) == 96, "ShadowView must match the shader's scalar layout");
struct FrameData {
    glm::vec3 cameraPosition;
    uint32_t lightCount;
    glm::vec3 skyAmbient;
    uint32_t debugView;
    glm::vec3 groundAmbient;
    uint32_t cascadeCount;
    glm::vec3 cameraForward;
    float shadowDistance;
    glm::vec4 cascadeEnds;   // view depth where each cascade ends
    uint32_t cascadeAtlas;   // bindless slots of the two shadow atlases
    uint32_t localAtlas;
    uint32_t shadowTaps;   // filter samples per lookup
    uint32_t brdfTable;   // bindless slot of image-based lighting's BRDF table
    VkDeviceAddress shadowViews;
    float specularLevels;   // mip levels of the prefiltered specular cube
    uint32_t aoTexture;   // bindless slot of the final ambient occlusion; 0: none this frame
    // The screen-space contract's (G03, Renderer/ScreenSpace.h): the camera's, unmodified.
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 inverseProjection;
    glm::vec2 renderSize;   // the scene targets' size in pixels
    float aoRadius;   // metres
    float aoIntensity;
    VkDeviceAddress reflectionProbes;   // the frame's ReflectionProbeData table (G07)
    uint32_t reflectionProbeCount;
    uint32_t pad;
};
static_assert(
    sizeof(FrameData) == 336 && offsetof(FrameData, shadowViews) == 96 && offsetof(FrameData, view) == 112 &&
        offsetof(FrameData, renderSize) == 304 && offsetof(FrameData, reflectionProbes) == 320,
    "FrameData must match the shader's scalar layout (Shaders/frame.glsl)");
// A local reflection probe as the shaders read it (G07). Must match ReflectionProbeData in
// shaders/reflection.glsl.
struct ReflectionProbeData {
    glm::mat4 worldToBox;   // rigid
    glm::vec3 halfExtents;   // metres
    float blendDistance;
    glm::vec3 capturePosition;
    uint32_t cube;   // slot in the cube table (binding 7); kNoCube: not loaded
    int32_t priority;
    uint32_t pad[3];
};
static_assert(sizeof(ReflectionProbeData) == 112, "ReflectionProbeData must match the shader's scalar layout");
constexpr uint32_t kNoCube = 0xFFFFFFFFu;
// Cube texture slots: room for every probe twice, so a new capture loads completely beside
// the one it replaces.
constexpr uint32_t kReflectionCubeSlots = 2 * kMaxReflectionProbes;
constexpr VkDeviceSize kShadowViewsOffset = sizeof(FrameData) + sizeof(LightData) * kMaxLights;
constexpr VkDeviceSize kReflectionProbesOffset = kShadowViewsOffset + sizeof(ShadowView) * kMaxShadowViews;
constexpr VkDeviceSize kFrameDataSize = kReflectionProbesOffset + sizeof(ReflectionProbeData) * kMaxReflectionProbes;

// Must match the push constants in shaders/shadow.vert and shadow.frag.
struct ShadowPushConstants {
    VkDeviceAddress drawBuffer;
    uint64_t pad;
    glm::mat4 viewProjection;
};
static_assert(sizeof(ShadowPushConstants) == 80, "ShadowPushConstants must match the shadow shaders");

struct DrawPushConstants {
    VkDeviceAddress drawBuffer;
    VkDeviceAddress frameBuffer;
};

// Must match the push constants in shaders/tonemap.frag.
struct TonemapPushConstants {
    glm::vec2 outputSize;
    uint32_t sceneTexture;
    float exposureScale;
    uint32_t tonemapper;
    uint32_t bloomTexture;   // the bloom chain's top level
    float bloomIntensity;   // 0: none
};

// Must match the push constants in shaders/grid.vert and grid.frag.
struct GridPushConstants {
    glm::mat4 viewProjection;
    glm::vec4 cameraPosition;   // xyz; w: half the quad's size (the fade distance)
    glm::vec4 params;   // x: cell size, y: cells per major line, z: opacity
};
static_assert(sizeof(GridPushConstants) == 96, "GridPushConstants must match the grid shaders");

// Must match the push constants in shaders/ui.vert and ui.frag.
struct UiPushConstants {
    glm::mat4 transform;   // output pixels to clip space, after any CSS transform
    VkDeviceAddress vertices;
    uint32_t texture;
    uint32_t pad;
};
static_assert(sizeof(UiPushConstants) == 80, "UiPushConstants must match the UI shaders");
// The game UI is blended in sRGB like a browser, so it draws through a UNORM view of the
// sRGB display image.
constexpr VkFormat kUiTargetFormat = VK_FORMAT_R8G8B8A8_UNORM;
// The Vulkan format a portable texture format uploads as (RendererOptimisation.cpp).
VkFormat vulkanFormat(TextureFormat format, bool srgb);

struct DrawCommand {
    DrawData data;
    glm::vec4 bounds;   // world-space bounding sphere: center, radius
    VkBuffer indexBuffer;
    uint32_t firstIndex = 0;   // in indexBuffer: the level of detail picked (pickLod)...
    uint32_t fullIndexCount = 0;   // ...and the full mesh's count when it's a simpler one
    uint32_t indexCount;
    uint32_t drawIndex = 0;   // its entry in the frame's draw table, once gathered
    // Skinned draws (RendererSkinning.cpp): which DrawList, and which SkinJob in it.
    uint32_t skinList = 0, skin = kNoSkin;
    // Draws with a detail or second layer: which LayerData in the same DrawList.
    uint32_t layers = kNoLayers;
    // Draws lit by probes: which ProbeData in the same DrawList.
    uint32_t probes = kNoLayers;
    bool doubleSided = false, blend = false, mirrored = false, castShadows = true;
    // In the camera's view. Draws outside it stay in the draw table: they may still cast
    // shadows into the view.
    bool visible = true;
    float depth = 0.0f;
    // Which object this is across frames (RenderTypes.h). drawIndex is only its place in
    // this frame's sorted table: per-object history must be found by key, never by index.
    RenderInstanceKey key;
};

// One shadow map to draw this frame: where in which atlas, from where, and which draws.
struct ShadowPass {
    glm::mat4 viewProjection;
    VkRect2D rect;
    float depthBias = 0;   // reverse-Z: negative pushes surfaces away from the light
    float slopeBias = 0;
    std::vector<uint32_t> draws;   // indices into gatheredDraws
};

// Shadow map settings for a ShadowQuality.
struct ShadowConfig {
    uint32_t cascadeSize = 0;   // one cascade's tile; the atlas holds 2 x 2
    uint32_t tileSize = 0;   // one spot light or point light face
    uint32_t localAtlasSize = 0;
    uint32_t taps = 0;
};
ShadowConfig shadowConfig(ShadowQuality quality);

// A skinned draw's vertices, skinned on the GPU before anything draws them.
struct SkinJob {
    uint32_t mesh;
    uint32_t firstJoint;   // in its DrawList's joints
    uint32_t jointCount;
};

// A view's six clip planes (Gribb & Hartmann), for testing bounding spheres. Works for any
// projection with 0..1 depth, reversed or not; a plane a projection doesn't have (the far
// plane of an infinite one) is left out.
// The camera's culls the scene's draws (drawMesh); each shadow map's culls its casters.
struct Frustum {
    std::array<glm::vec4, 6> planes{};
    uint32_t count = 0;

    explicit Frustum(const glm::mat4& m) {
        auto row = [&](int i) { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
        const glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        for (const glm::vec4& p : {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2}) {
            const float length = glm::length(glm::vec3(p));
            if (length > 1e-6f) planes[count++] = p / length;
        }
    }
    bool intersects(const glm::vec4& sphere) const {
        for (uint32_t i = 0; i < count; ++i) {
            if (glm::dot(glm::vec3(planes[i]), glm::vec3(sphere)) + planes[i].w < -sphere.w) return false;
        }
        return true;
    }
};

// One per thread so drawMesh never takes a lock on the hot path.
struct alignas(64) DrawList {
    std::vector<DrawCommand> commands;
    std::vector<SkinJob> skins;
    std::vector<glm::mat4> joints;
    std::vector<LayerData> layers;
    std::vector<ProbeData> probes;
};

// Must match the push constants in shaders/skinning.comp.
struct SkinningPushConstants {
    VkDeviceAddress source;   // the mesh's vertices
    VkDeviceAddress skin;   // its VertexSkins
    VkDeviceAddress joints;   // this draw's joint matrices
    VkDeviceAddress target;   // where the skinned vertices go
    uint32_t vertexCount;
    uint32_t jointCount;
};
static_assert(sizeof(SkinningPushConstants) == 40, "SkinningPushConstants must match skinning.comp");

struct Mesh {
    vk::Buffer vertices;
    vk::Buffer indices;
    VkDeviceAddress vertexAddress = 0;
    // Simpler levels of detail (setMeshLods): ranges of their own index buffer.
    vk::Buffer lodIndices;
    std::array<MeshLod, kMaxMeshLods> lods{};
    uint32_t lodCount = 0;
    uint32_t indexCount = 0;
    glm::vec4 bounds{0.0f};   // bounding sphere in mesh space: center, radius
    // Skinned meshes only: one VertexSkin per vertex (setMeshSkin).
    vk::Buffer skin;
    VkDeviceAddress skinAddress = 0;
    uint32_t vertexCount = 0;
    // A second UV set (setMeshUv1), one vec2 per vertex.
    vk::Buffer uv1;
    VkDeviceAddress uv1Address = 0;
    // Baked receivers' lightmap UVs (setMeshLightmapUv), one vec2 per vertex.
    vk::Buffer lightmapUv;
    VkDeviceAddress lightmapUvAddress = 0;
};

// Records a layout transition with synchronization2.
inline void transitionImage(
    VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
    uint32_t baseMip = 0, uint32_t mipCount = VK_REMAINING_MIP_LEVELS, uint32_t layerCount = 1) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, baseMip, mipCount, 0, layerCount};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

inline std::string shaderPath(const char* name) {
    // Shaders are compiled next to the executable (build/bin/shaders), so lookup
    // doesn't depend on the working directory.
    return std::string(SDL_GetBasePath()) + "shaders/" + name + ".spv";
}

}   // namespace detail

using namespace detail;

struct VulkanRenderer::Impl {
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore imageAcquired = VK_NULL_HANDLE;   // binary: swapchain acquire requires it
        vk::Buffer drawBuffer;   // this frame's draw table, persistently mapped
        DrawData* drawData = nullptr;
        VkDeviceAddress drawAddress = 0;
        uint32_t drawCapacity = 0;
        vk::Buffer layerBuffer;   // this frame's LayerData, persistently mapped
        LayerData* layerData = nullptr;
        VkDeviceAddress layerAddress = 0;
        uint32_t layerCapacity = 0;
        vk::Buffer probeBuffer;   // this frame's ProbeData, persistently mapped
        ProbeData* probeData = nullptr;
        VkDeviceAddress probeAddress = 0;
        uint32_t probeCapacity = 0;
        vk::Buffer frameBuffer;   // lights and camera, persistently mapped
        uint8_t* frameData = nullptr;
        VkDeviceAddress frameAddress = 0;
        VkQueryPool timestamps = VK_NULL_HANDLE;   // GPU timers for this frame's passes
        std::vector<std::string> timedPasses;   // what they timed, in order; empty: nothing to read
        // Game UI geometry, written by drawUi while the GPU is done with this slot.
        vk::Buffer uiVertexBuffer, uiIndexBuffer;
        UiVertex* uiVertices = nullptr;
        uint32_t* uiIndices = nullptr;
        VkDeviceAddress uiVertexAddress = 0;
        size_t uiVertexCapacity = 0, uiIndexCapacity = 0;
        // Skinning (RendererSkinning.cpp): this frame's joint matrices, written by the CPU,
        // and the skinned vertices the compute pass writes for the scene and shadows.
        vk::Buffer jointBuffer, skinnedBuffer;
        vk::BufferState skinnedState;   // the render graph's, reset when the buffer is replaced
        glm::mat4* joints = nullptr;
        VkDeviceAddress jointAddress = 0, skinnedAddress = 0;
        size_t jointCapacity = 0, skinnedCapacity = 0;   // matrices; vertices
    };

    Window& window;
    JobSystem& jobs;
    std::unique_ptr<vk::Context> context;
    std::unique_ptr<vk::Swapchain> swapchain;
    std::array<Frame, kFramesInFlight> frames{};
    // One per swapchain image: presentation may still be reading a semaphore
    // after its frame finishes on the GPU, so these can't be per frame-in-flight.
    std::vector<VkSemaphore> renderFinished;

    // Timeline semaphore pacing: frame N signals value N when the GPU finishes it.
    // Before reusing a frame slot the CPU waits for value N - kFramesInFlight.
    VkSemaphore frameTimeline = VK_NULL_HANDLE;
    uint64_t frameNumber = 0;

    // Frame pacing: frame N is presented with present id N, so present wait can tell when
    // it reached the screen.
    VkSwapchainKHR presentedSwapchain = VK_NULL_HANDLE;   // the swapchain frameNumber went to
    uint64_t lastPresentTicks = 0;   // SDL performance counter when the last wait returned
    std::vector<PassTiming> passTimings;
    float gpuFrameMs = 0;

    // Bindless texture table: one descriptor set, bound once per frame.
    VkDescriptorSetLayout bindlessLayout = VK_NULL_HANDLE;
    VkDescriptorPool bindlessPool = VK_NULL_HANDLE;
    VkDescriptorSet bindlessSet = VK_NULL_HANDLE;
    VkSampler linearSampler = VK_NULL_HANDLE;
    std::mutex descriptorMutex;   // descriptor writes need host synchronization

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline meshPipeline = VK_NULL_HANDLE;
    VkPipeline meshDoubleSidedPipeline = VK_NULL_HANDLE;
    VkPipeline meshBlendPipeline = VK_NULL_HANDLE;
    VkPipeline meshBlendDoubleSidedPipeline = VK_NULL_HANDLE;
    std::array<VkPipeline, 4> mirroredPipelines{};
    // Opaque without alpha test, so with early depth testing: + 1 double-sided, + 2 mirrored.
    std::array<VkPipeline, 4> solidPipelines{};
    // With the surface prepass (G03): the prepass itself, and the opaque shading over it
    // (depth EQUAL, no writes, reading ambient occlusion). Indexed by prepassIndex.
    std::array<VkPipeline, 8> prepassPipelines{};
    std::array<VkPipeline, 8> depthEqualPipelines{};
    static uint32_t prepassIndex(const DrawCommand& draw) {
        return (draw.data.alphaCutoff >= 0.0f ? 4 : 0) + (draw.mirrored ? 2 : 0) + (draw.doubleSided ? 1 : 0);
    }
    VkPipeline meshPipelineFor(const DrawCommand& draw) const;
    VkPipelineLayout gridLayout = VK_NULL_HANDLE;
    VkPipeline gridPipeline = VK_NULL_HANDLE;
    GridSettings grid;
    VkPipelineLayout tonemapLayout = VK_NULL_HANDLE;
    VkPipeline tonemapPipeline = VK_NULL_HANDLE;

    // Shadows: one atlas for the sun's cascades, one for spot and point lights, both
    // sampled through the bindless table with a comparison sampler.
    ShadowQuality shadowQuality = ShadowQuality::High;
    ShadowConfig shadowSettings;
    ShadowQuality builtShadowQuality = ShadowQuality::Off;   // what the atlases in use were made for
    bool needsShadowRebuild = false;
    vk::Image cascadeAtlas, localAtlas;
    vk::ImageState cascadeState, localState;
    uint32_t cascadeSlot = 0, localSlot = 0;   // the pair in use
    // Two pairs of slots: new atlases take the pair the old ones (still read by frames in
    // flight) aren't using, and the old ones are retired, so a quality change never waits
    // for the GPU.
    uint32_t shadowSlots[2][2] = {};
    int shadowPair = 0;
    bool shadowPairBusy[2] = {false, false};
    // At a frame boundary: atlases for shadowQuality, the old ones retired. False while
    // both pairs are still in flight (the next frame tries again).
    bool rebuildShadowMaps();
    VkSampler shadowSampler = VK_NULL_HANDLE;
    VkPipelineLayout shadowLayout = VK_NULL_HANDLE;
    VkPipeline shadowPipeline = VK_NULL_HANDLE;   // alpha-tested casters
    VkPipeline shadowDepthOnlyPipeline = VK_NULL_HANDLE;   // every other caster
    std::vector<ShadowPass> cascadePasses, localPasses;   // this frame's
    ShadowStats shadowStats;

    // Lighting for the frame being recorded.
    std::mutex lightMutex;
    std::vector<LightDesc> lights;
    LightingDesc lighting;
    DebugView debugView = DebugView::Lit;
    uint32_t lastLightCount = 0, lastDroppedLights = 0;
    // Levels of detail (RendererOptimisation.cpp): set on the main thread before drawing.
    LodSettings lodSettings;
    LodStats lastLodStats;

    // The frame's passes, rebuilt every frame; image states carry over between frames.
    vk::RenderGraph graph;
    vk::ImageState sceneColorState, sceneDepthState, displayState;
    std::string frameGraphText;
    uint32_t untimedPasses = 0;   // passes past the GPU timer's capacity last frame

    // A screenshot asked for by requestScreenshot, taken at the end of the next frame.
    std::string screenshotPath;
    bool screenshotRequested = false;
    std::string screenshotMessage;

    // Fixed-capacity resource slots. A slot is fully written before its ready flag is
    // set (release), so readers on any thread need no lock.
    std::unique_ptr<Mesh[]> meshes = std::make_unique<Mesh[]>(kMaxMeshes);
    std::unique_ptr<std::atomic<bool>[]> meshReady = std::make_unique<std::atomic<bool>[]>(kMaxMeshes);
    // Indexed by the ids Renderer hands out (RenderBackend.h). Texture 0 is white; the
    // renderer's own bindless slots (scene, shadow atlases) sit at the top of the table.
    std::unique_ptr<vk::Image[]> textures;
    // Slots in the bindless texture array: the renderer's own are the last kInternalTextureSlots,
    // with kSpareTextureSlots below them.
    uint32_t bindlessSlots() const { return context->maxBindlessTextures() - kEnvironmentCubes - kReflectionCubeSlots; }
    uint32_t spareTextureSlots() const { return std::min(kSpareTextureSlots, bindlessSlots() / 8); }
    uint32_t textureCapacity() const { return bindlessSlots() - kInternalTextureSlots - spareTextureSlots(); }
    // Which bindless slot each texture id's current image is in: its own id until it's
    // replaced, then a spare. Shaders get slots, never ids (textureSlot()).
    static constexpr uint32_t kNoSlot = UINT32_MAX;
    std::unique_ptr<std::atomic<uint32_t>[]> textureSlots;
    std::mutex textureSlotMutex;
    std::vector<uint32_t> freeTextureSlots;   // spares, and slots whose old image has been freed
    uint32_t textureSlot(uint32_t id) const {
        const uint32_t slot = id < textureCapacity() ? textureSlots[id].load(std::memory_order_acquire) : kNoSlot;
        return slot == kNoSlot ? 0 : slot;   // 0: white
    }
    uint32_t takeTextureSlot();   // kNoSlot when none is free
    void publishTexture(uint32_t id, vk::Image& image);

    // ---- Local reflection probes (G07, RendererReflections.cpp) ----
    // Cube textures (TextureShape::Cube) live in a table of their own, binding 7: the slot of
    // each texture id that is one, kNoSlot for the rest. textures[id] holds the image.
    std::unique_ptr<std::atomic<uint32_t>[]> cubeSlots;
    std::vector<uint32_t> freeCubeSlots;   // under textureSlotMutex
    uint32_t cubeSlot(uint32_t id) const {
        return id < textureCapacity() ? cubeSlots[id].load(std::memory_order_acquire) : kNoSlot;
    }
    vk::Image uploadCube(const TextureData& data);
    void publishCube(uint32_t id, vk::Image& image);
    void writeCubeDescriptor(uint32_t slot, VkImageView view);
    // The scene's probes (setReflectionProbes): replaced whole on the main thread, read by
    // drawMesh on any thread.
    std::shared_ptr<const std::vector<ReflectionProbeDesc>> reflectionProbes =
        std::make_shared<const std::vector<ReflectionProbeDesc>>();
    std::atomic<bool> localReflections{true};   // the reflection_probes setting
    void setReflectionProbes(std::span<const ReflectionProbeDesc> probes);
    // The probes a draw with these bounds reads, packed for DrawData::reflections.
    uint32_t pickReflections(const glm::vec4& bounds) const;
    uint32_t writeReflectionProbes(Frame& f);   // into the frame buffer; how many
    // Reflection capture (Renderer::requestSceneCapture): the next frame's HDR scene colour,
    // copied after the scene pass and read back once the frame is done.
    std::atomic<bool> captureRequested{false};
    SceneCapture capture;
    vk::Buffer captureBuffer;
    VkExtent2D captureExtent{};
    void recordCaptureCopy(VkCommandBuffer cmd);
    void finishCapture();

    // ---- Material samplers (G02) ----
    // Indexed by SamplerHandle; slot id + bank * kMaxSamplers in binding 6.
    std::mutex samplerMutex;
    std::vector<SamplerDesc> samplerDescs{SamplerDesc{}};
    std::array<std::array<VkSampler, kMaxSamplers>, kSamplerBanks> samplerObjects{};
    uint32_t samplerBank = 0;
    bool samplerBankBusy[kSamplerBanks] = {false, false};
    uint32_t anisotropy = 16;   // the setting
    std::atomic<bool> materialDetail{true};   // material_quality high (A02)
    bool needsSamplerRebuild = false;
    float effectiveAnisotropy(const SamplerDesc& desc) const;
    VkSampler makeSampler(const SamplerDesc& desc);
    void writeSamplerDescriptor(uint32_t slot, VkSampler sampler);
    void createSampler(SamplerHandle handle, const SamplerDesc& desc);
    void setAnisotropy(uint32_t value);
    void rebuildSamplers();   // at a frame boundary, after setAnisotropy
    void destroySamplers();
    uint32_t samplerSlot(SamplerHandle handle) const {
        return (handle.id < kMaxSamplers ? handle.id : 0) + samplerBank * kMaxSamplers;
    }
    void setMeshUv1(MeshHandle handle, std::span<const glm::vec2> uv1);
    void setMeshLightmapUv(MeshHandle handle, std::span<const glm::vec2> uv);

    std::mutex queueMutex;   // VkQueue access must be externally synchronized
    VkCommandPool uploadPool = VK_NULL_HANDLE;

    std::vector<DrawList> drawLists;   // indexed by JobSystem::threadIndex()
    std::mutex foreignDrawMutex;
    DrawList foreignDraws;   // threads the job system doesn't own
    uint32_t lastDrawCount = 0;
    uint32_t lastCulledDraws = 0;   // of those, outside the camera's view
    uint32_t lastUnkeyedDraws = 0;   // of those, without a RenderInstanceKey
    std::vector<DrawCommand> gatheredDraws;

    vk::Image sceneColor;
    vk::Image sceneDepth;
    vk::Image displayColor;
    uint32_t sceneSlot = 0;   // sceneColor's place in the bindless table, for the tonemap pass
    float renderScale = 1.0f;
    // Editor viewport size; when set, the scene is sized from it instead of the window
    // and handed to the GUI as a texture.
    VkExtent2D viewportSize{0, 0};
    bool needsSceneResize = false;

    // Dear ImGui, drawn on top of everything into the swapchain image.
    bool guiEnabled = false;
    VkFormat guiColorFormat = VK_FORMAT_UNDEFINED;
    VkDescriptorSet guiSceneTexture = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, VkDescriptorSet> guiTextures;   // texture id -> ImTextureID, main thread

    // Reloaded textures, uploaded by any thread and swapped in at the start of a frame.
    struct TextureSwap {
        uint32_t id;
        vk::Image image;
    };
    std::mutex textureSwapMutex;
    std::vector<TextureSwap> textureSwaps;

    uint32_t frameIndex = 0;
    uint32_t imageIndex = 0;
    bool needsResize = false;
    Color clearColor{0.02f, 0.02f, 0.03f, 1.0f};
    glm::mat4 viewProjection{1.0f};
    Frustum viewFrustum{glm::mat4(1.0f)};   // viewProjection's, set with it
    glm::mat4 view{1.0f}, projection{1.0f};
    glm::vec3 cameraPosition{0.0f};

    Impl(Window& w, JobSystem& j, bool vsync);

    ~Impl();

    // ---- Setup ------------------------------------------------------------------------

    void checkBlitSupport();

    vk::Image createImage(
        VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t mipLevels = 1,
        bool dedicated = false, VkImageCreateFlags flags = 0);

    void destroyImage(vk::Image& image);

    // Where the scene ends up: the editor viewport if there is one, otherwise the window.
    bool viewportMode() const { return guiEnabled && viewportSize.width && viewportSize.height; }
    VkExtent2D outputExtent() const { return viewportMode() ? viewportSize : swapchain->extent(); }

    void createRenderTargets();

    // Only called with the GPU idle, so the old descriptor set is no longer in use.
    void registerGuiSceneTexture();

    void recreateSceneTargets();

    // ---- GUI --------------------------------------------------------------------------

    void initGui();

    void shutdownGui();

    // Draws this frame's ImGui output into the swapchain image, which must be in
    // COLOR_ATTACHMENT_OPTIMAL. clear: start from a blank image instead of the scene.
    void renderGuiWindows();
    void recordGui(VkCommandBuffer cmd, VkImageView target, bool clear);

    // ---- Frames, descriptors and pipelines ---------------------------------------------

    void createFrames();

    // Grows a frame's draw table. Only called when the GPU is done with that frame.
    void ensureDrawCapacity(Frame& f, uint32_t count);
    void ensureLayerCapacity(Frame& f, uint32_t count);
    void ensureProbeCapacity(Frame& f, uint32_t count);

    void createRenderFinishedSemaphores();

    void destroyRenderFinishedSemaphores();

    void createBindless();

    void createPipelines();

    // ---- Resources --------------------------------------------------------------------

    vk::Buffer createBuffer(
        VkDeviceSize size, VkBufferUsageFlags usage, VmaAllocationCreateFlags flags,
        VmaAllocationInfo* outInfo = nullptr);

    VkDeviceAddress bufferAddress(VkBuffer buffer);

    vk::Buffer createStaging(const void* data, VkDeviceSize size);

    // Records commands with `record`, submits them and waits. Safe from any thread.
    // A dedicated transfer queue with async uploads comes with asset streaming.
    void immediateSubmit(const std::function<void(VkCommandBuffer)>& record);

    vk::Buffer uploadBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage);

    void createMesh(MeshHandle id, std::span<const Vertex> vertices, std::span<const uint32_t> indices);
    void destroyMesh(MeshHandle handle);
    void destroyTexture(TextureHandle handle);
    void createTexture(TextureHandle id, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb);
    bool isTexture(uint32_t id) const { return id < textureCapacity() && textures[id].image; }

    void
    updateTexture(TextureHandle texture, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb);

    // At a frame boundary: each new image goes into a spare slot and the draws from this frame
    // on use it; the old image and slot are retired, freed once the frames reading them
    // finish. Nothing waits for the GPU unless every spare is still in flight.
    void applyTextureSwaps();

    uint64_t guiTexture(TextureHandle texture);

    void writeTextureDescriptor(uint32_t slot, VkImageView view);

    // Uploads RGBA8 pixels into a new sampled image with a full mip chain.
    vk::Image uploadTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb);
    // The same for any uncompressed format the GPU can blit (RGBA16F, say).
    vk::Image uploadTexture(uint32_t width, uint32_t height, std::span<const uint8_t> pixels, VkFormat format);

    // ---- Optimised assets (RendererOptimisation.cpp) -----------------------------------
    void setMeshLods(MeshHandle handle, std::span<const MeshLod> lods, std::span<const uint32_t> lodIndices);
    bool supportsTextureFormat(TextureFormat format) const;
    void createTextureData(TextureHandle handle, const TextureData& data);
    void updateTextureData(TextureHandle handle, const TextureData& data);
    // Uploads every level as given; a lone uncompressed level gets its mipmaps from uploadTexture.
    vk::Image uploadTextureData(const TextureData& data);
    // The level of detail to draw a mesh with: its first index and count. scale: the
    // draw's largest axis scale; center: its bounding sphere's centre in the world.
    void pickLod(const Mesh& mesh, float scale, const glm::vec4& bounds, DrawCommand& command) const;
    bool formatSampleable(VkFormat format) const;

    // ---- Per-frame --------------------------------------------------------------------

    // joints: a skinned draw's joint matrices (drawSkinnedMesh); empty for a plain one.
    void drawMesh(
        MeshHandle handle, const glm::mat4& transform, TextureHandle texture, const glm::vec4& tint,
        const DrawMaterial& material, const RenderInstanceKey& key, std::span<const glm::mat4> joints = {});

    void clearDrawLists();

    void recreateSwapchain();

    bool beginFrame();
    void readTimestamps(Frame& f);
    float waitForPresent();

    void setSceneViewport(VkCommandBuffer cmd);

    // One quad on y = 0 following the camera; the fragment shader draws the lines and
    // fades them out with distance.
    void recordGrid(VkCommandBuffer cmd);

    // Gathers every thread's draws into this frame's GPU draw table, sorted for drawing.
    void prepareDraws(Frame& f);
    // Records the gathered draws into the scene.
    void recordDraws(VkCommandBuffer cmd, Frame& f);

    // ---- Skinning (RendererSkinning.cpp) -----------------------------------------------
    // Skinned draws are skinned by a compute pass at the start of the frame into a
    // per-frame vertex buffer; the scene and shadow passes then draw those vertices like
    // any mesh's.

    struct SkinDispatch {
        SkinningPushConstants push;
    };
    VkPipelineLayout skinningLayout = VK_NULL_HANDLE;
    VkPipeline skinningPipeline = VK_NULL_HANDLE;
    std::vector<SkinDispatch> skinDispatches;   // this frame's
    uint32_t skinnedVertexCount = 0;   // last frame's

    void createSkinningPipeline();
    void destroySkinning();
    void setMeshSkin(MeshHandle handle, std::span<const VertexSkin> skin);
    void drawSkinnedMesh(
        MeshHandle handle, const glm::mat4& transform, std::span<const glm::mat4> joints, TextureHandle texture,
        const glm::vec4& tint, const DrawMaterial& material, const RenderInstanceKey& key);
    // A sphere around every place the joints can move the mesh's vertices to.
    static glm::vec4 skinnedBounds(const Mesh& mesh, std::span<const glm::mat4> joints);
    // Adds a draw to a thread's list, with its joints when it's skinned.
    static void
    addDraw(DrawList& list, uint32_t listIndex, DrawCommand& command, uint32_t mesh, std::span<const glm::mat4> joints);
    // After gathering: uploads the joints, plans the dispatches and points each skinned
    // draw at its skinned vertices.
    void prepareSkinning(Frame& f);
    void recordSkinning(VkCommandBuffer cmd);

    // ---- Shadows (RendererShadows.cpp) -------------------------------------------------

    void createShadowMaps();
    void destroyShadowMaps();
    // Picks the shadow casters among this frame's lights, fits their shadow maps, culls
    // the draws for each, and fills the lights' shadow fields and the ShadowViews.
    void planShadows(
        const std::vector<LightDesc>& descs, LightData* lightData, uint32_t lightCount, ShadowView* views,
        FrameData& header);
    // Draws one atlas's shadow maps.
    void recordShadows(VkCommandBuffer cmd, Frame& f, const std::vector<ShadowPass>& passes, const vk::Image& atlas);

    // ---- Particles (RendererParticles.cpp) --------------------------------------------

    // Must match ParticleBatchData in shaders/particle.frag.
    struct ParticleBatchData {
        glm::vec4 tint;
        glm::vec4 edgeColor;
        glm::vec3 lightDirection;
        uint32_t texture;
        glm::vec3 lightColor;
        uint32_t alphaTexture;
        glm::vec3 ambient;
        uint32_t flags;
        float emissive;
        uint32_t blend;
        float pad[2];
    };
    static_assert(sizeof(ParticleBatchData) == 96, "ParticleBatchData must match shaders/particle.frag");
    struct ParticleDraw {
        uint32_t firstIndex, indexCount;
        ParticleBlend blend;
        ParticleCull cull;
        bool depthTest, depthWrite;
    };
    // Per frame in flight, persistently mapped, grown (only) when a frame doesn't fit.
    struct ParticleBuffers {
        vk::Buffer vertices, indices, batches;
        void *vertexData = nullptr, *indexData = nullptr, *batchData = nullptr;
        size_t vertexCapacity = 0, indexCapacity = 0, batchCapacity = 0;   // bytes
        VkDeviceAddress vertexAddress = 0, batchAddress = 0;
    };
    std::array<ParticleBuffers, kFramesInFlight> particleBuffers{};
    VkPipelineLayout particleLayout = VK_NULL_HANDLE;
    std::array<VkPipeline, 5> particlePipelines{};   // indexed by ParticleBlend
    // This frame's particles, gathered by drawParticles; capacity is kept between frames.
    std::vector<ParticleVertex> particleVertices;
    std::vector<uint32_t> particleIndices;
    std::vector<ParticleBatchData> particleBatchData;
    std::vector<ParticleDraw> particleDraws;
    float particleGatherMs = 0;   // this frame's drawParticles calls, added up
    ParticleStats lastParticleStats;   // set by prepareParticles

    void createParticlePipelines();
    void destroyParticles();
    void drawParticles(const ParticleDrawList& list);
    void clearParticles();
    void prepareParticles();   // copies the frame's particles to the GPU
    void recordParticles(VkCommandBuffer cmd);

    void endFrame();

    // Camera, ambient light and this frame's lights, where the mesh shader reads them.
    void writeFrameData(Frame& f);

    // The frame as a render graph: scene, tonemap, then to the screen (and the GUI).
    void buildGraph(VkCommandBuffer, Frame& f, VkImage swapchainImage);

    void recordScene(VkCommandBuffer cmd, Frame& f);

    void recordTonemap(VkCommandBuffer cmd);

    void recordCopyToWindow(VkCommandBuffer cmd, VkImage swapchainImage);

    // ---- Game UI overlay (RendererUi.cpp) --------------------------------------------

    VkPipelineLayout uiLayout = VK_NULL_HANDLE;
    VkPipeline uiPipeline = VK_NULL_HANDLE;
    VkImageView displayUiView = VK_NULL_HANDLE;   // displayColor as UNORM, for sRGB blending
    std::vector<UiDrawCommand> uiCommands;   // this frame's; capacity is kept between frames
    std::vector<glm::mat4> uiTransforms;
    glm::uvec2 uiLayoutSize{0};

    void createUiPipeline();
    void destroyUiPipeline();
    void createDisplayUiView();
    void destroyDisplayUiView();
    // Copies the list into this frame's buffers; grows them (only) when it doesn't fit.
    void drawUi(const UiDrawList& list);
    void recordUi(VkCommandBuffer cmd, Frame& f);
    void destroyUiBuffers(Frame& f);

    // ---- Image-based lighting (RendererEnvironment.cpp) -------------------------------
    // The surroundings (an environment map, or the sky and ground colors) are drawn into a
    // cube map, then filtered into the light a surface receives from them: a small
    // irradiance cube for diffuse light and a prefiltered specular cube whose mip levels
    // hold blurrier reflections for rougher surfaces. A BRDF table made at startup finishes
    // the split-sum approximation in mesh.frag. Rebuilt only when the environment changes.

    struct CubeMap {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;   // a cube with every level, for sampling
        std::vector<VkImageView> levelViews;   // one 2D array (six faces) per level, for writing
        std::vector<VkDescriptorSet> levelTargets;   // a storage image set per level view
        uint32_t size = 0, levels = 0;
    };
    // What the cubes were last made from; anything else makes them again.
    struct EnvironmentKey {
        VkImage map = VK_NULL_HANDLE;
        glm::vec3 sky{0.0f}, ground{0.0f};
        float intensity = 0.0f, rotation = 0.0f;
        bool operator==(const EnvironmentKey&) const = default;
    };
    CubeMap environmentCube, specularCube, irradianceCube;
    vk::Image brdfTable;
    VkImageView brdfTableTarget = VK_NULL_HANDLE;   // brdfTable as a one-layer 2D array
    VkDescriptorSet brdfTableSet = VK_NULL_HANDLE;
    uint32_t brdfSlot = 0;
    VkDescriptorSetLayout environmentTargetLayout = VK_NULL_HANDLE;
    VkDescriptorPool environmentPool = VK_NULL_HANDLE;
    VkPipelineLayout environmentLayout = VK_NULL_HANDLE;
    VkPipeline environmentSkyPipeline = VK_NULL_HANDLE, environmentFilterPipeline = VK_NULL_HANDLE;
    VkPipelineLayout skyLayout = VK_NULL_HANDLE;
    VkPipeline skyPipeline = VK_NULL_HANDLE;
    EnvironmentKey environmentKey;
    bool environmentBuilt = false;

    void createEnvironment();
    void destroyEnvironment();
    CubeMap createCubeMap(uint32_t size, uint32_t levels);
    void destroyCubeMap(CubeMap& cube);
    // The environment map to use: its image, or null for the sky and ground colors.
    VkImage environmentMapImage() const;
    // Whether the cubes need making again for this frame's lighting (and remembers it).
    bool environmentChanged();
    void recordEnvironment(VkCommandBuffer cmd);
    // The environment map behind everything, before the scene's draws.
    bool showsSky() const;
    void recordSky(VkCommandBuffer cmd);

    // ---- Bloom (RendererBloom.cpp) ----------------------------------------------------
    // Bright light spreads into its surroundings, the way it does in a camera lens. The HDR
    // scene is filtered down a chain of half-size levels and back up again, each level
    // adding a wider blur, and the tonemap pass mixes the result in.

    // One chain of levels, with the descriptors that read and write it. A quality change
    // makes a new chain and retires the old one (retire()), so nothing waits for the GPU.
    struct BloomChain {
        vk::Image image;   // the first level's size, then halving per level
        std::vector<VkImageView> levelViews;
        std::vector<VkExtent2D> extents;
        std::vector<VkDescriptorSet> downSets, upSets;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        uint32_t slot = 0;   // bindless slot the tonemap pass reads the top level from
        BloomQuality quality = BloomQuality::Off;
    };
    BloomChain bloom;   // the one in use; no image when bloom is off
    BloomQuality bloomQuality = BloomQuality::High;   // asked for
    float bloomScale = 1.0f;   // the player's multiplier on the scene's intensity
    uint32_t bloomSlots[2] = {0, 0};
    bool bloomSlotBusy[2] = {false, false};   // a retired chain still owns it
    VkSampler bloomSampler = VK_NULL_HANDLE;   // linear, clamped to the edges
    VkDescriptorSetLayout bloomSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout bloomLayout = VK_NULL_HANDLE;
    VkPipeline bloomPipeline = VK_NULL_HANDLE;

    void createBloomPipeline();
    void destroyBloomPipeline();
    // A chain for bloomQuality at the scene's size, in a free slot; false when both slots
    // are still owned by chains in flight (try again next frame).
    bool createBloomTargets();
    void destroyBloomChain(BloomChain& chain);   // the GPU must be done with it
    void destroyBloomTargets();   // now, with the GPU idle
    // At a frame boundary: a chain matching bloomQuality, the old one retired.
    void updateBloomTargets();
    float bloomIntensity() const;   // this frame's; 0 when off or in a debug view
    void recordBloom(VkCommandBuffer cmd);

    // ---- Anti-aliasing (RendererAntiAliasing.cpp) -------------------------------------
    // FXAA after tone mapping and before game UI: the tonemap pass writes aaInput instead
    // of the display image, and FXAA writes the display image from it.

    struct AaTarget {
        vk::Image image;   // kDisplayFormat at output size; tonemap writes it as sRGB
        VkImageView encodedView = VK_NULL_HANDLE;   // the same bytes as UNORM: FXAA reads encoded values
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    AaTarget aaTarget;   // no image while anti-aliasing is none
    vk::ImageState aaState;
    AntiAliasing antiAliasing = AntiAliasing::None;   // asked for
    VkSampler aaSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout aaSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout fxaaLayout = VK_NULL_HANDLE;
    VkPipeline fxaaPipeline = VK_NULL_HANDLE;

    void createAntiAliasingPipeline();
    void destroyAntiAliasingPipeline();
    void createAaTarget();
    void destroyAaTarget(AaTarget& target);   // the GPU must be done with it
    void updateAaTarget();   // at a frame boundary: retire or make aaTarget to match antiAliasing
    bool fxaaActive() const;   // this frame runs FXAA
    void recordFxaa(VkCommandBuffer cmd);

    // ---- Surface prepass and ambient occlusion (RendererAmbientOcclusion.cpp) ---------
    // With ambient occlusion on, opaque surfaces are drawn twice: first a prepass writing
    // depth and view-space normals, then the shading, which tests depth for EQUAL. Between
    // them a compute pass works out the occlusion at half resolution and blurs it, and the
    // shading upsamples it (shaders/mesh.frag). Off, none of it exists.

    struct AoTargets {
        vk::Image normal;   // full resolution, octahedral view-space normals
        vk::Image ao[2];   // half resolution (occlusion, view distance, normal); ao[0] is the result
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet sets[3] = {};   // occlusion into ao[0], blur ao[0] -> ao[1], ao[1] -> ao[0]
        uint32_t slot = 0;   // bindless slot of ao[0]
    };
    AoTargets aoTargets;   // no images while it's off
    vk::ImageState aoNormalState;
    AmbientOcclusion ambientOcclusion = AmbientOcclusion::Off;   // asked for
    VkFormat aoNormalFormat = VK_FORMAT_R16G16_SFLOAT;
    uint32_t aoSlots[2] = {0, 0};
    bool aoSlotBusy[2] = {false, false};
    VkSampler aoSampler = VK_NULL_HANDLE;   // nearest, clamped
    VkDescriptorSetLayout aoSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout aoLayout = VK_NULL_HANDLE;
    VkPipeline aoPipeline = VK_NULL_HANDLE;

    void createAmbientOcclusionPipeline();
    void destroyAmbientOcclusionPipeline();
    // The quality this frame computes: the setting, or medium for the debug view.
    AmbientOcclusion effectiveAmbientOcclusion() const;
    bool createAoTargets();   // false while both slots are in flight
    void destroyAoTargets(AoTargets& targets);   // the GPU must be done with them
    void updateAoTargets();   // at a frame boundary
    bool prepassActive() const { return aoTargets.normal.image != VK_NULL_HANDLE; }
    void recordPrepass(VkCommandBuffer cmd, Frame& f);
    void recordAmbientOcclusion(VkCommandBuffer cmd, Frame& f);

    // ---- Retiring resources -----------------------------------------------------------
    // Freed once every frame submitted so far has finished, checked at each frame start.
    // Turning an effect off releases its images this way, without waiting for the GPU.
    struct Retired {
        uint64_t afterFrame;   // frameTimeline value that means the GPU is done with it
        std::function<void()> destroy;
    };
    std::vector<Retired> retired;
    void retire(std::function<void()> destroy);
    void collectRetired(bool all);   // all: the GPU is idle (shutdown, resize)

    // ---- Screenshots ------------------------------------------------------------------

    vk::Buffer screenshotBuffer;
    VkExtent2D screenshotExtent{};

    void recordScreenshotCopy(VkCommandBuffer cmd);

    // Waits for the frame that copied the image, then writes the PNG.
    void finishScreenshot();

    vk::ImageState swapchainState;

    // ---- Identity and reports (RendererReports.cpp) -----------------------------------

    RenderViewId currentView;   // the view being drawn (Renderer::setCamera)
    uint32_t viewChanges = 0;   // times the view changed: each one starts history afresh
    std::string loggedFallbacks;   // the settings fallbacks last logged

    void setView(RenderViewId id);
    RendererCapabilities capabilities() const;
    std::vector<EffectiveSetting> effectiveSettings() const;
    // Logs requested settings the device can't honour, once each time that changes.
    void logSettingFallbacks();
    RendererMemoryReport memoryReport() const;
};

}   // namespace shockwave
