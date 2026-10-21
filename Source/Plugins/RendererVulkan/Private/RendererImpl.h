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
constexpr uint32_t kInternalTextureSlots = 5;   // scene color, two shadow atlases, BRDF table, bloom
// Image-based lighting's cube maps (environment, specular, irradiance) sit beside the
// bindless table in the same set, so they come out of the same descriptor budget.
constexpr uint32_t kEnvironmentCubes = 3;
// The scene renders in HDR; tonemapping to the display happens at the end of the frame.
constexpr VkFormat kSceneColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
// The tonemapped result at output resolution: what the window or editor viewport shows.
constexpr VkFormat kDisplayFormat = VK_FORMAT_R8G8B8A8_SRGB;

// One entry per draw in the frame's draw table. Must match DrawData in shaders/mesh.vert.
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
    uint32_t pad;
};
static_assert(
    offsetof(DrawData, vertexBuffer) == 216 && offsetof(DrawData, emissive) == 240 &&
        offsetof(DrawData, normalTexture) == 252 && offsetof(DrawData, occlusionStrength) == 272 &&
        sizeof(DrawData) == 280,
    "DrawData must match the shader's scalar layout");

// Shadow maps hold reverse-Z depth, like the scene: cleared to 0, closer is greater.
constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;
constexpr uint32_t kMaxCascades = 4;
constexpr uint32_t kMaxLocalShadowViews = 64;   // tiles in the spot/point light atlas
constexpr uint32_t kMaxShadowViews = kMaxCascades + kMaxLocalShadowViews;
constexpr uint32_t kNoShadow = 0xFFFFFFFFu;
constexpr uint32_t kNoSkin = 0xFFFFFFFFu;

// Must match LightData and FrameBuffer in shaders/mesh.frag.
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
    uint32_t pad;
};
static_assert(
    sizeof(FrameData) == 112 && offsetof(FrameData, shadowViews) == 96,
    "FrameData must match the shader's scalar layout");
constexpr VkDeviceSize kShadowViewsOffset = sizeof(FrameData) + sizeof(LightData) * kMaxLights;
constexpr VkDeviceSize kFrameDataSize = kShadowViewsOffset + sizeof(ShadowView) * kMaxShadowViews;

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
    bool doubleSided = false, blend = false, mirrored = false, castShadows = true;
    // In the camera's view. Draws outside it stay in the draw table: they may still cast
    // shadows into the view.
    bool visible = true;
    float depth = 0.0f;
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
    bool needsShadowRebuild = false;
    vk::Image cascadeAtlas, localAtlas;
    vk::ImageState cascadeState, localState;
    uint32_t cascadeSlot = 0, localSlot = 0;
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
    // Slots in the bindless texture array: the renderer's own are the last kInternalTextureSlots.
    uint32_t bindlessSlots() const { return context->maxBindlessTextures() - kEnvironmentCubes; }
    uint32_t textureCapacity() const { return bindlessSlots() - kInternalTextureSlots; }

    std::mutex queueMutex;   // VkQueue access must be externally synchronized
    VkCommandPool uploadPool = VK_NULL_HANDLE;

    std::vector<DrawList> drawLists;   // indexed by JobSystem::threadIndex()
    std::mutex foreignDrawMutex;
    DrawList foreignDraws;   // threads the job system doesn't own
    uint32_t lastDrawCount = 0;
    uint32_t lastCulledDraws = 0;   // of those, outside the camera's view
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

    // Frames in flight may still be sampling the old images, so wait for the GPU to
    // finish them before swapping. Reloads are rare (a file changed), so a short stall is fine.
    void applyTextureSwaps();

    uint64_t guiTexture(TextureHandle texture);

    void writeTextureDescriptor(uint32_t id);

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
        const DrawMaterial& material, std::span<const glm::mat4> joints = {});

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
        const glm::vec4& tint, const DrawMaterial& material);
    // A sphere around every place the joints can move the mesh's vertices to.
    static glm::vec4 skinnedBounds(const Mesh& mesh, std::span<const glm::mat4> joints);
    // Adds a draw to a thread's list, with its joints when it's skinned.
    static void addDraw(
        DrawList& list, uint32_t listIndex, DrawCommand& command, uint32_t mesh, std::span<const glm::mat4> joints);
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

    vk::Image bloomChain;   // half the scene's size, then halving per level
    std::vector<VkImageView> bloomLevelViews;
    std::vector<VkExtent2D> bloomExtents;
    std::vector<VkDescriptorSet> bloomDownSets, bloomUpSets;
    VkSampler bloomSampler = VK_NULL_HANDLE;   // linear, clamped to the edges
    VkDescriptorSetLayout bloomSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool bloomPool = VK_NULL_HANDLE;
    VkPipelineLayout bloomLayout = VK_NULL_HANDLE;
    VkPipeline bloomPipeline = VK_NULL_HANDLE;
    uint32_t bloomSlot = 0;

    void createBloomPipeline();
    void destroyBloomPipeline();
    // With the scene's render targets, at their size.
    void createBloomTargets();
    void destroyBloomTargets();
    float bloomIntensity() const;   // this frame's; 0 when off or in a debug view
    void recordBloom(VkCommandBuffer cmd);

    // ---- Screenshots ------------------------------------------------------------------

    vk::Buffer screenshotBuffer;
    VkExtent2D screenshotExtent{};

    void recordScreenshotCopy(VkCommandBuffer cmd);

    // Waits for the frame that copied the image, then writes the PNG.
    void finishScreenshot();

    vk::ImageState swapchainState;
};

}   // namespace shockwave
