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
constexpr uint32_t kInternalTextureSlots = 3;   // scene color and the two shadow atlases
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
    uint32_t pad;
};
static_assert(
    offsetof(DrawData, vertexBuffer) == 216 && offsetof(DrawData, emissive) == 240 && sizeof(DrawData) == 256,
    "DrawData must match the shader's scalar layout");

// Shadow maps hold reverse-Z depth, like the scene: cleared to 0, closer is greater.
constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;
constexpr uint32_t kMaxCascades = 4;
constexpr uint32_t kMaxLocalShadowViews = 64;   // tiles in the spot/point light atlas
constexpr uint32_t kMaxShadowViews = kMaxCascades + kMaxLocalShadowViews;
constexpr uint32_t kNoShadow = 0xFFFFFFFFu;

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
    float pad;
    VkDeviceAddress shadowViews;
    uint64_t pad2;
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
    uint32_t indexCount;
    uint32_t drawIndex = 0;   // its entry in the frame's draw table, once gathered
    bool doubleSided = false, blend = false, mirrored = false, castShadows = true;
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

// One per thread so drawMesh never takes a lock on the hot path.
struct alignas(64) DrawList {
    std::vector<DrawCommand> commands;
};

struct Mesh {
    vk::Buffer vertices;
    vk::Buffer indices;
    VkDeviceAddress vertexAddress = 0;
    uint32_t indexCount = 0;
    glm::vec4 bounds{0.0f};   // bounding sphere in mesh space: center, radius
};

// Records a layout transition with synchronization2.
inline void transitionImage(
    VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
    uint32_t baseMip = 0, uint32_t mipCount = VK_REMAINING_MIP_LEVELS) {
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
    barrier.subresourceRange = {aspect, baseMip, mipCount, 0, 1};

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
    VkPipeline shadowPipeline = VK_NULL_HANDLE;
    std::vector<ShadowPass> cascadePasses, localPasses;   // this frame's
    ShadowStats shadowStats;

    // Lighting for the frame being recorded.
    std::mutex lightMutex;
    std::vector<LightDesc> lights;
    LightingDesc lighting;
    DebugView debugView = DebugView::Lit;
    uint32_t lastLightCount = 0, lastDroppedLights = 0;

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
    uint32_t textureCapacity() const { return context->maxBindlessTextures() - kInternalTextureSlots; }

    std::mutex queueMutex;   // VkQueue access must be externally synchronized
    VkCommandPool uploadPool = VK_NULL_HANDLE;

    std::vector<DrawList> drawLists;   // indexed by JobSystem::threadIndex()
    std::mutex foreignDrawMutex;
    DrawList foreignDraws;   // threads the job system doesn't own
    uint32_t lastDrawCount = 0;
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

    // ---- Per-frame --------------------------------------------------------------------

    void drawMesh(
        MeshHandle handle, const glm::mat4& transform, TextureHandle texture, const glm::vec4& tint,
        const DrawMaterial& material);

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

    // ---- Screenshots ------------------------------------------------------------------

    vk::Buffer screenshotBuffer;
    VkExtent2D screenshotExtent{};

    void recordScreenshotCopy(VkCommandBuffer cmd);

    // Waits for the frame that copied the image, then writes the PNG.
    void finishScreenshot();

    vk::ImageState swapchainState;
};

}   // namespace shockwave
