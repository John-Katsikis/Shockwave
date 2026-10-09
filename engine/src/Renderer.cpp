#include <shockwave/Jobs.h>
#include <shockwave/Renderer.h>
#include <shockwave/Window.h>

#include "vulkan/Context.h"
#include "vulkan/Pipeline.h"
#include "vulkan/Swapchain.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_vulkan.h>

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
#include <vector>

namespace shockwave {

namespace {

constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kMaxMeshes = 65536;
// The scene renders in HDR; tonemapping to the display happens at the end of the frame.
constexpr VkFormat kSceneColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

// One entry per draw in the frame's draw table. Must match DrawData in shaders/mesh.vert.
struct DrawData {
    glm::mat4 mvp;
    glm::mat4 normalMatrix;
    glm::vec4 tint;
    VkDeviceAddress vertexBuffer;
    uint32_t textureIndex;
    uint32_t pad;
};
static_assert(offsetof(DrawData, vertexBuffer) == 144 && sizeof(DrawData) == 160,
              "DrawData must match the shader's scalar layout");

struct DrawPushConstants {
    VkDeviceAddress drawBuffer;
};

struct DrawCommand {
    DrawData data;
    VkBuffer indexBuffer;
    uint32_t indexCount;
};

// One per thread so drawMesh never takes a lock on the hot path.
struct alignas(64) DrawList {
    std::vector<DrawCommand> commands;
};

struct Mesh {
    vk::Buffer vertices;
    vk::Buffer indices;
    VkDeviceAddress vertexAddress = 0;
    uint32_t indexCount = 0;
};

// Records a layout transition with synchronization2.
void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from,
                     VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                     VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, uint32_t baseMip = 0,
                     uint32_t mipCount = VK_REMAINING_MIP_LEVELS) {
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

std::string shaderPath(const char* name) {
    // Shaders are compiled next to the executable (build/bin/shaders), so lookup
    // doesn't depend on the working directory.
    return std::string(SDL_GetBasePath()) + "shaders/" + name + ".spv";
}

} // namespace

struct Renderer::Impl {
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore imageAcquired = VK_NULL_HANDLE;   // binary: swapchain acquire requires it
        vk::Buffer drawBuffer;                        // this frame's draw table, persistently mapped
        DrawData* drawData = nullptr;
        VkDeviceAddress drawAddress = 0;
        uint32_t drawCapacity = 0;
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

    // Bindless texture table: one descriptor set, bound once per frame.
    VkDescriptorSetLayout bindlessLayout = VK_NULL_HANDLE;
    VkDescriptorPool bindlessPool = VK_NULL_HANDLE;
    VkDescriptorSet bindlessSet = VK_NULL_HANDLE;
    VkSampler linearSampler = VK_NULL_HANDLE;
    std::mutex descriptorMutex;   // descriptor writes need host synchronization

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline meshPipeline = VK_NULL_HANDLE;

    // Fixed-capacity resource slots. A slot is fully written before its ready flag is
    // set (release), so readers on any thread need no lock.
    std::unique_ptr<Mesh[]> meshes = std::make_unique<Mesh[]>(kMaxMeshes);
    std::unique_ptr<std::atomic<bool>[]> meshReady = std::make_unique<std::atomic<bool>[]>(kMaxMeshes);
    std::atomic<uint32_t> meshReserved{0};
    std::unique_ptr<vk::Image[]> textures;
    std::atomic<uint32_t> textureReserved{0};

    std::mutex queueMutex;   // VkQueue access must be externally synchronized
    VkCommandPool uploadPool = VK_NULL_HANDLE;

    std::vector<DrawList> drawLists;        // indexed by JobSystem::threadIndex()
    std::mutex foreignDrawMutex;
    DrawList foreignDraws;                  // threads the job system doesn't own
    uint32_t lastDrawCount = 0;

    vk::Image sceneColor;
    vk::Image sceneDepth;
    float renderScale = 1.0f;
    // Editor viewport size; when set, the scene is sized from it instead of the window
    // and handed to the GUI as a texture.
    VkExtent2D viewportSize{0, 0};
    bool needsSceneResize = false;

    // Dear ImGui, drawn on top of everything into the swapchain image.
    bool guiEnabled = false;
    VkFormat guiColorFormat = VK_FORMAT_UNDEFINED;
    VkDescriptorSet guiSceneTexture = VK_NULL_HANDLE;

    uint32_t frameIndex = 0;
    uint32_t imageIndex = 0;
    bool needsResize = false;
    Color clearColor{0.02f, 0.02f, 0.03f, 1.0f};
    glm::mat4 viewProjection{1.0f};

    Impl(Window& w, JobSystem& j, bool vsync) : window(w), jobs(j) {
        context = std::make_unique<vk::Context>(window.handle());
        swapchain = std::make_unique<vk::Swapchain>(*context, window.pixelWidth(), window.pixelHeight(), vsync);
        drawLists.resize(jobs.threadCount());
        textures = std::make_unique<vk::Image[]>(context->maxBindlessTextures());

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = context->graphicsQueueFamily();
        vk::check(vkCreateCommandPool(context->device(), &poolInfo, nullptr, &uploadPool), "vkCreateCommandPool");

        checkBlitSupport();
        createRenderTargets();
        createFrames();
        createRenderFinishedSemaphores();
        createBindless();
        createPipelines();

        const uint8_t white[4] = {255, 255, 255, 255};
        createTexture(1, 1, white, false);   // slot 0: the default texture
    }

    ~Impl() {
        VkDevice device = context->device();
        VmaAllocator allocator = context->allocator();
        vkDeviceWaitIdle(device);
        if (guiEnabled) shutdownGui();
        for (uint32_t i = 0; i < std::min(meshReserved.load(), kMaxMeshes); ++i) {
            if (!meshReady[i].load()) continue;
            vmaDestroyBuffer(allocator, meshes[i].vertices.buffer, meshes[i].vertices.allocation);
            vmaDestroyBuffer(allocator, meshes[i].indices.buffer, meshes[i].indices.allocation);
        }
        const uint32_t textureCount = std::min(textureReserved.load(), context->maxBindlessTextures());
        for (uint32_t i = 0; i < textureCount; ++i) destroyImage(textures[i]);
        vkDestroyCommandPool(device, uploadPool, nullptr);
        destroyImage(sceneColor);
        destroyImage(sceneDepth);
        vkDestroyPipeline(device, meshPipeline, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroySampler(device, linearSampler, nullptr);
        vkDestroyDescriptorPool(device, bindlessPool, nullptr);
        vkDestroyDescriptorSetLayout(device, bindlessLayout, nullptr);
        destroyRenderFinishedSemaphores();
        vkDestroySemaphore(device, frameTimeline, nullptr);
        for (Frame& f : frames) {
            vmaDestroyBuffer(allocator, f.drawBuffer.buffer, f.drawBuffer.allocation);
            vkDestroySemaphore(device, f.imageAcquired, nullptr);
            vkDestroyCommandPool(device, f.pool, nullptr);
        }
        swapchain.reset();
        context.reset();
    }

    // ---- Setup ------------------------------------------------------------------------

    void checkBlitSupport() {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(context->physicalDevice(), swapchain->format(), &props);
        if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
            throw std::runtime_error("The swapchain format can't be a blit target on this GPU");
        }
    }

    vk::Image createImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                          uint32_t mipLevels = 1, bool dedicated = false) {
        vk::Image image;
        image.extent = extent;
        image.format = format;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {extent.width, extent.height, 1};
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (dedicated) allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        vk::check(vmaCreateImage(context->allocator(), &imageInfo, &allocInfo, &image.image, &image.allocation,
                                 nullptr),
                  "vmaCreateImage");

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {aspect, 0, mipLevels, 0, 1};
        vk::check(vkCreateImageView(context->device(), &viewInfo, nullptr, &image.view), "vkCreateImageView");
        return image;
    }

    void destroyImage(vk::Image& image) {
        if (image.view) vkDestroyImageView(context->device(), image.view, nullptr);
        if (image.image) vmaDestroyImage(context->allocator(), image.image, image.allocation);
        image = {};
    }

    // Where the scene ends up: the editor viewport if there is one, otherwise the window.
    bool viewportMode() const { return guiEnabled && viewportSize.width && viewportSize.height; }
    VkExtent2D outputExtent() const { return viewportMode() ? viewportSize : swapchain->extent(); }

    void createRenderTargets() {
        const VkExtent2D out = outputExtent();
        const VkExtent2D extent{
            std::max(1u, static_cast<uint32_t>(std::lround(out.width * renderScale))),
            std::max(1u, static_cast<uint32_t>(std::lround(out.height * renderScale))),
        };
        sceneColor = createImage(extent, kSceneColorFormat,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT, 1, true);
        sceneDepth = createImage(extent, kDepthFormat,
                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                 VK_IMAGE_ASPECT_DEPTH_BIT, 1, true);
        registerGuiSceneTexture();
    }

    // Only called with the GPU idle, so the old descriptor set is no longer in use.
    void registerGuiSceneTexture() {
        if (!guiEnabled) return;
        if (guiSceneTexture) ImGui_ImplVulkan_RemoveTexture(guiSceneTexture);
        guiSceneTexture = ImGui_ImplVulkan_AddTexture(sceneColor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    void recreateSceneTargets() {
        vkDeviceWaitIdle(context->device());
        destroyImage(sceneColor);
        destroyImage(sceneDepth);
        createRenderTargets();
        needsSceneResize = false;
    }

    // ---- GUI --------------------------------------------------------------------------

    void initGui() {
        guiColorFormat = swapchain->format();

        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_3;
        info.Instance = context->instance();
        info.PhysicalDevice = context->physicalDevice();
        info.Device = context->device();
        info.QueueFamily = context->graphicsQueueFamily();
        info.Queue = context->graphicsQueue();
        info.DescriptorPoolSize = 256;   // the backend owns its pool: one set per GUI texture
        info.MinImageCount = 2;
        // ImGui rotates its vertex buffers per frame; it needs at least one per frame in flight.
        info.ImageCount = std::max(kFramesInFlight, swapchain->imageCount());
        info.UseDynamicRendering = true;
        info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &guiColorFormat;
        info.CheckVkResultFn = [](VkResult result) { vk::check(result, "Dear ImGui Vulkan backend"); };
        if (!ImGui_ImplVulkan_Init(&info)) throw std::runtime_error("Initializing the Dear ImGui Vulkan backend failed");

        guiEnabled = true;
        registerGuiSceneTexture();
    }

    void shutdownGui() {
        if (!guiEnabled) return;
        vkDeviceWaitIdle(context->device());
        if (guiSceneTexture) ImGui_ImplVulkan_RemoveTexture(guiSceneTexture);
        guiSceneTexture = VK_NULL_HANDLE;
        ImGui_ImplVulkan_Shutdown();
        guiEnabled = false;
        if (viewportSize.width) needsSceneResize = true;   // back to window-sized output
    }

    // Draws this frame's ImGui output into the swapchain image, which must be in
    // COLOR_ATTACHMENT_OPTIMAL. clear: start from a blank image instead of the scene.
    void recordGui(VkCommandBuffer cmd, VkImageView target, bool clear) {
        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = target;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, swapchain->extent()};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &rendering);
        if (ImDrawData* data = ImGui::GetDrawData()) {
            // The backend may upload font and UI textures on the queue from inside this call.
            std::lock_guard lock(queueMutex);
            ImGui_ImplVulkan_RenderDrawData(data, cmd);
        }
        vkCmdEndRendering(cmd);
    }

    void createFrames() {
        VkDevice device = context->device();
        for (Frame& f : frames) {
            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            poolInfo.queueFamilyIndex = context->graphicsQueueFamily();
            vk::check(vkCreateCommandPool(device, &poolInfo, nullptr, &f.pool), "vkCreateCommandPool");

            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = f.pool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            vk::check(vkAllocateCommandBuffers(device, &allocInfo, &f.cmd), "vkAllocateCommandBuffers");

            VkSemaphoreCreateInfo semInfo{};
            semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            vk::check(vkCreateSemaphore(device, &semInfo, nullptr, &f.imageAcquired), "vkCreateSemaphore");

            ensureDrawCapacity(f, 4096);
        }

        VkSemaphoreTypeCreateInfo timelineInfo{};
        timelineInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timelineInfo.initialValue = 0;
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semInfo.pNext = &timelineInfo;
        vk::check(vkCreateSemaphore(device, &semInfo, nullptr, &frameTimeline), "vkCreateSemaphore");
    }

    // Grows a frame's draw table. Only called when the GPU is done with that frame.
    void ensureDrawCapacity(Frame& f, uint32_t count) {
        if (count <= f.drawCapacity) return;
        uint32_t capacity = std::max<uint32_t>(f.drawCapacity, 4096);
        while (capacity < count) capacity *= 2;

        if (f.drawBuffer.buffer) vmaDestroyBuffer(context->allocator(), f.drawBuffer.buffer, f.drawBuffer.allocation);
        VmaAllocationInfo info{};
        // Host-written, GPU-read every frame: lands in BAR/ReBAR memory where the GPU has it.
        f.drawBuffer = createBuffer(capacity * sizeof(DrawData),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                        VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                    &info);
        f.drawData = static_cast<DrawData*>(info.pMappedData);
        f.drawAddress = bufferAddress(f.drawBuffer.buffer);
        f.drawCapacity = capacity;
    }

    void createRenderFinishedSemaphores() {
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        renderFinished.resize(swapchain->imageCount());
        for (VkSemaphore& s : renderFinished) {
            vk::check(vkCreateSemaphore(context->device(), &semInfo, nullptr, &s), "vkCreateSemaphore");
        }
    }

    void destroyRenderFinishedSemaphores() {
        for (VkSemaphore s : renderFinished) vkDestroySemaphore(context->device(), s, nullptr);
        renderFinished.clear();
    }

    void createBindless() {
        VkDevice device = context->device();
        const uint32_t capacity = context->maxBindlessTextures();

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.anisotropyEnable = VK_TRUE;
        samplerInfo.maxAnisotropy = std::min(16.0f, context->maxAnisotropy());
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        vk::check(vkCreateSampler(device, &samplerInfo, nullptr, &linearSampler), "vkCreateSampler");

        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[0].descriptorCount = capacity;
        bindings[0].stageFlags = VK_SHADER_STAGE_ALL;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_ALL;
        bindings[1].pImmutableSamplers = &linearSampler;

        // Partially bound: unused slots may be empty. Update-after-bind + unused-while-pending:
        // new textures can be added while frames using the table are still in flight.
        const VkDescriptorBindingFlags bindingFlags[2] = {
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
            0,
        };
        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
        flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flagsInfo.bindingCount = 2;
        flagsInfo.pBindingFlags = bindingFlags;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.pNext = &flagsInfo;
        layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings = bindings;
        vk::check(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &bindlessLayout),
                  "vkCreateDescriptorSetLayout");

        const VkDescriptorPoolSize sizes[2] = {
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = sizes;
        vk::check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &bindlessPool), "vkCreateDescriptorPool");

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = bindlessPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &bindlessLayout;
        vk::check(vkAllocateDescriptorSets(device, &allocInfo, &bindlessSet), "vkAllocateDescriptorSets");
    }

    void createPipelines() {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.size = sizeof(DrawPushConstants);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &bindlessLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &range;
        vk::check(vkCreatePipelineLayout(context->device(), &layoutInfo, nullptr, &pipelineLayout),
                  "vkCreatePipelineLayout");

        vk::GraphicsPipelineDesc desc;
        desc.vertexShader = shaderPath("mesh.vert");
        desc.fragmentShader = shaderPath("mesh.frag");
        desc.colorFormat = kSceneColorFormat;
        desc.depthFormat = kDepthFormat;
        desc.layout = pipelineLayout;
        meshPipeline = vk::createGraphicsPipeline(context->device(), desc);
    }

    // ---- Resources --------------------------------------------------------------------

    vk::Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaAllocationCreateFlags flags,
                            VmaAllocationInfo* outInfo = nullptr) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = flags;

        vk::Buffer buffer;
        vk::check(vmaCreateBuffer(context->allocator(), &bufferInfo, &allocInfo, &buffer.buffer,
                                  &buffer.allocation, outInfo),
                  "vmaCreateBuffer");
        return buffer;
    }

    VkDeviceAddress bufferAddress(VkBuffer buffer) {
        VkBufferDeviceAddressInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        info.buffer = buffer;
        return vkGetBufferDeviceAddress(context->device(), &info);
    }

    vk::Buffer createStaging(const void* data, VkDeviceSize size) {
        VmaAllocationInfo info{};
        vk::Buffer staging = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                              VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                          &info);
        std::memcpy(info.pMappedData, data, size);
        vk::check(vmaFlushAllocation(context->allocator(), staging.allocation, 0, VK_WHOLE_SIZE),
                  "vmaFlushAllocation");
        return staging;
    }

    // Records commands with `record`, submits them and waits. Safe from any thread.
    // A dedicated transfer queue with async uploads comes with asset streaming.
    void immediateSubmit(const std::function<void(VkCommandBuffer)>& record) {
        std::lock_guard lock(queueMutex);
        VkDevice device = context->device();

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = uploadPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vk::check(vkAllocateCommandBuffers(device, &allocInfo, &cmd), "vkAllocateCommandBuffers");

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk::check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
        record(cmd);
        vk::check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        vk::check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence");

        VkCommandBufferSubmitInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdInfo.commandBuffer = cmd;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        vk::check(vkQueueSubmit2(context->graphicsQueue(), 1, &submit, fence), "vkQueueSubmit2");
        vk::check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");

        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, uploadPool, 1, &cmd);
    }

    vk::Buffer uploadBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
        vk::Buffer staging = createStaging(data, size);
        vk::Buffer buffer = createBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0);
        immediateSubmit([&](VkCommandBuffer cmd) {
            VkBufferCopy region{0, 0, size};
            vkCmdCopyBuffer(cmd, staging.buffer, buffer.buffer, 1, &region);
        });
        vmaDestroyBuffer(context->allocator(), staging.buffer, staging.allocation);
        return buffer;
    }

    MeshHandle createMesh(std::span<const Vertex> vertices, std::span<const uint32_t> indices) {
        if (vertices.empty() || indices.empty()) return {};
        const uint32_t id = meshReserved.fetch_add(1);
        if (id >= kMaxMeshes) throw std::runtime_error("Out of mesh slots");

        Mesh& mesh = meshes[id];
        mesh.vertices = uploadBuffer(vertices.data(), vertices.size_bytes(),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        mesh.indices = uploadBuffer(indices.data(), indices.size_bytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        mesh.indexCount = static_cast<uint32_t>(indices.size());
        mesh.vertexAddress = bufferAddress(mesh.vertices.buffer);

        meshReady[id].store(true, std::memory_order_release);
        return MeshHandle{id};
    }

    TextureHandle createTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) {
        if (width == 0 || height == 0 || rgba.size() < size_t(width) * height * 4) {
            throw std::runtime_error("createTexture: pixel data is smaller than width x height x 4");
        }
        const uint32_t id = textureReserved.fetch_add(1);
        if (id >= context->maxBindlessTextures()) throw std::runtime_error("Out of bindless texture slots");

        const uint32_t mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
        const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        vk::Image image = createImage({width, height}, format,
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                      VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);

        vk::Buffer staging = createStaging(rgba.data(), size_t(width) * height * 4);
        immediateSubmit([&](VkCommandBuffer cmd) {
            transitionImage(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                            VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                            VK_ACCESS_2_TRANSFER_WRITE_BIT);   // level 0 is copied, the rest are blitted

            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {width, height, 1};
            vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

            // Build the mip chain on the GPU: each level is a filtered blit of the one above.
            int32_t w = static_cast<int32_t>(width), h = static_cast<int32_t>(height);
            for (uint32_t level = 1; level < mipLevels; ++level) {
                transitionImage(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                VK_ACCESS_2_TRANSFER_READ_BIT, level - 1, 1);
                const int32_t nw = std::max(1, w / 2), nh = std::max(1, h / 2);
                VkImageBlit2 region{};
                region.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
                region.srcOffsets[1] = {w, h, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                region.dstOffsets[1] = {nw, nh, 1};
                VkBlitImageInfo2 blit{};
                blit.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
                blit.srcImage = image.image;
                blit.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                blit.dstImage = image.image;
                blit.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                blit.regionCount = 1;
                blit.pRegions = &region;
                blit.filter = VK_FILTER_LINEAR;
                vkCmdBlitImage2(cmd, &blit);
                w = nw;
                h = nh;
            }

            // Every level but the last is in TRANSFER_SRC now; the last is in TRANSFER_DST.
            if (mipLevels > 1) {
                transitionImage(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                0, mipLevels - 1);
            }
            transitionImage(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, mipLevels - 1, 1);
        });
        vmaDestroyBuffer(context->allocator(), staging.buffer, staging.allocation);

        textures[id] = image;
        {
            std::lock_guard lock(descriptorMutex);
            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageView = image.view;
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = bindlessSet;
            write.dstBinding = 0;
            write.dstArrayElement = id;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            write.pImageInfo = &imageInfo;
            vkUpdateDescriptorSets(context->device(), 1, &write, 0, nullptr);
        }
        return TextureHandle{id};
    }

    // ---- Per-frame --------------------------------------------------------------------

    void drawMesh(MeshHandle handle, const glm::mat4& transform, TextureHandle texture, const glm::vec4& tint) {
        if (handle.id >= kMaxMeshes || !meshReady[handle.id].load(std::memory_order_acquire)) return;
        const Mesh& mesh = meshes[handle.id];

        // The per-draw math runs here, on whichever thread submits the draw.
        DrawCommand command;
        command.data.mvp = viewProjection * transform;
        command.data.normalMatrix = glm::transpose(glm::inverse(transform));
        command.data.tint = tint;
        command.data.vertexBuffer = mesh.vertexAddress;
        command.data.textureIndex = texture.id;
        command.data.pad = 0;
        command.indexBuffer = mesh.indices.buffer;
        command.indexCount = mesh.indexCount;

        const uint32_t thread = JobSystem::threadIndex();
        if (thread < drawLists.size()) {
            drawLists[thread].commands.push_back(command);
        } else {
            std::lock_guard lock(foreignDrawMutex);
            foreignDraws.commands.push_back(command);
        }
    }

    void clearDrawLists() {
        for (DrawList& list : drawLists) list.commands.clear();
        std::lock_guard lock(foreignDrawMutex);
        foreignDraws.commands.clear();
    }

    void recreateSwapchain() {
        vkDeviceWaitIdle(context->device());
        destroyRenderFinishedSemaphores();
        destroyImage(sceneColor);
        destroyImage(sceneDepth);
        swapchain->recreate(window.pixelWidth(), window.pixelHeight());
        createRenderTargets();
        createRenderFinishedSemaphores();
        needsResize = false;
        needsSceneResize = false;
    }

    bool beginFrame() {
        clearDrawLists();
        if (window.isMinimized()) return false;
        if (needsResize) {
            recreateSwapchain();
        } else if (needsSceneResize) {
            recreateSceneTargets();
        }

        VkDevice device = context->device();
        Frame& f = frames[frameIndex];

        // Wait until the GPU has finished the frame that last used this slot.
        if (frameNumber >= kFramesInFlight) {
            const uint64_t waitValue = frameNumber + 1 - kFramesInFlight;
            VkSemaphoreWaitInfo waitInfo{};
            waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            waitInfo.semaphoreCount = 1;
            waitInfo.pSemaphores = &frameTimeline;
            waitInfo.pValues = &waitValue;
            vk::check(vkWaitSemaphores(device, &waitInfo, UINT64_MAX), "vkWaitSemaphores");
        }

        VkResult acquire = vkAcquireNextImageKHR(device, swapchain->handle(), UINT64_MAX,
                                                 f.imageAcquired, VK_NULL_HANDLE, &imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            needsResize = true;
            return false;
        }
        if (acquire != VK_SUBOPTIMAL_KHR) vk::check(acquire, "vkAcquireNextImageKHR");
        return true;
    }

    // Gathers every thread's draws into this frame's GPU draw table and records them.
    void recordDraws(VkCommandBuffer cmd, Frame& f) {
        uint32_t total = 0;
        for (const DrawList& list : drawLists) total += static_cast<uint32_t>(list.commands.size());
        total += static_cast<uint32_t>(foreignDraws.commands.size());
        lastDrawCount = total;
        if (total == 0) return;
        ensureDrawCapacity(f, total);

        const VkExtent2D extent = sceneColor.extent;
        // Negative height flips Y so standard Y-up projection matrices work unchanged.
        VkViewport viewport{0.0f, static_cast<float>(extent.height), static_cast<float>(extent.width),
                            -static_cast<float>(extent.height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &bindlessSet, 0, nullptr);
        DrawPushConstants push{f.drawAddress};
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

        uint32_t drawIndex = 0;
        VkBuffer boundIndices = VK_NULL_HANDLE;
        auto record = [&](const DrawList& list) {
            for (const DrawCommand& draw : list.commands) {
                f.drawData[drawIndex] = draw.data;
                if (draw.indexBuffer != boundIndices) {
                    vkCmdBindIndexBuffer(cmd, draw.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                    boundIndices = draw.indexBuffer;
                }
                // firstInstance carries the draw's slot in the table (gl_InstanceIndex).
                vkCmdDrawIndexed(cmd, draw.indexCount, 1, 0, 0, drawIndex);
                ++drawIndex;
            }
        };
        for (const DrawList& list : drawLists) record(list);
        {
            std::lock_guard lock(foreignDrawMutex);
            record(foreignDraws);
        }
        vk::check(vmaFlushAllocation(context->allocator(), f.drawBuffer.allocation, 0,
                                     sizeof(DrawData) * drawIndex),
                  "vmaFlushAllocation");
    }

    void endFrame() {
        Frame& f = frames[frameIndex];
        VkImage image = swapchain->image(imageIndex);
        VkCommandBuffer cmd = f.cmd;

        vk::check(vkResetCommandPool(context->device(), f.pool, 0), "vkResetCommandPool");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk::check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");

        // 1. Render the scene into the HDR target at render resolution.
        //    Source stages: last frame's blit or GUI pass may still be reading this image.
        transitionImage(cmd, sceneColor.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        transitionImage(cmd, sceneDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = sceneColor.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{clearColor.r, clearColor.g, clearColor.b, clearColor.a}};

        VkRenderingAttachmentInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = sceneDepth.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {0.0f, 0};   // reverse-Z: 0 is infinitely far

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, sceneColor.extent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        rendering.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &rendering);
        recordDraws(cmd, f);
        vkCmdEndRendering(cmd);

        // How the swapchain image was last touched, for the final transition to present.
        VkPipelineStageFlags2 lastStage;
        VkAccessFlags2 lastAccess;
        VkImageLayout lastLayout;

        if (viewportMode()) {
            // 2a. Editor: the GUI samples the scene inside its viewport panel.
            transitionImage(cmd, sceneColor.image, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            transitionImage(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            recordGui(cmd, swapchain->imageView(imageIndex), true);
            lastStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            lastAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            lastLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        } else {
            // 2b. Scale the scene to the window. A blit for now; the tonemap and upscaler
            //     pass (FSR-style) replaces this step without changing anything above it.
            transitionImage(cmd, sceneColor.image, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            transitionImage(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, 0,
                            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

            const VkExtent2D out = swapchain->extent();
            VkImageBlit2 region{};
            region.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.srcOffsets[1] = {static_cast<int32_t>(sceneColor.extent.width),
                                    static_cast<int32_t>(sceneColor.extent.height), 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstOffsets[1] = {static_cast<int32_t>(out.width), static_cast<int32_t>(out.height), 1};

            VkBlitImageInfo2 blit{};
            blit.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
            blit.srcImage = sceneColor.image;
            blit.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            blit.dstImage = image;
            blit.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            blit.regionCount = 1;
            blit.pRegions = &region;
            blit.filter = VK_FILTER_LINEAR;
            vkCmdBlitImage2(cmd, &blit);
            lastStage = VK_PIPELINE_STAGE_2_BLIT_BIT;
            lastAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            lastLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

            // 3. In-game GUI on top of the scene.
            if (guiEnabled) {
                transitionImage(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                recordGui(cmd, swapchain->imageView(imageIndex), false);
                lastStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                lastAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                lastLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
        }

        // Destination stage must match the renderFinished signal below, so the
        // transition is ordered before presentation.
        transitionImage(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, lastLayout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        lastStage, lastAccess, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);

        vk::check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

        ++frameNumber;
        VkSemaphore finished = renderFinished[imageIndex];

        VkSemaphoreSubmitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waitInfo.semaphore = f.imageAcquired;
        // First use of the swapchain image: the blit, or the GUI pass in editor mode.
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signalInfos[2]{};
        signalInfos[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signalInfos[0].semaphore = finished;
        signalInfos[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signalInfos[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signalInfos[1].semaphore = frameTimeline;
        signalInfos[1].value = frameNumber;
        signalInfos[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkCommandBufferSubmitInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdInfo.commandBuffer = cmd;

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &waitInfo;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        submit.signalSemaphoreInfoCount = 2;
        submit.pSignalSemaphoreInfos = signalInfos;

        VkSwapchainKHR sc = swapchain->handle();
        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &finished;
        present.swapchainCount = 1;
        present.pSwapchains = &sc;
        present.pImageIndices = &imageIndex;

        VkResult result;
        {
            std::lock_guard lock(queueMutex);
            vk::check(vkQueueSubmit2(context->graphicsQueue(), 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2");
            result = vkQueuePresentKHR(context->graphicsQueue(), &present);
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            needsResize = true;
        } else {
            vk::check(result, "vkQueuePresentKHR");
        }

        frameIndex = (frameIndex + 1) % kFramesInFlight;
    }
};

Renderer::Renderer(Window& window, JobSystem& jobs, bool vsync)
    : m_impl(std::make_unique<Impl>(window, jobs, vsync)) {}
Renderer::~Renderer() = default;

MeshHandle Renderer::createMesh(std::span<const Vertex> vertices, std::span<const uint32_t> indices) {
    return m_impl->createMesh(vertices, indices);
}
TextureHandle Renderer::createTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) {
    return m_impl->createTexture(width, height, rgba, srgb);
}
void Renderer::setClearColor(const Color& color) { m_impl->clearColor = color; }
void Renderer::setCamera(const glm::mat4& view, const glm::mat4& projection) {
    m_impl->viewProjection = projection * view;
}
void Renderer::drawMesh(MeshHandle mesh, const glm::mat4& transform, TextureHandle texture, const glm::vec4& tint) {
    m_impl->drawMesh(mesh, transform, texture, tint);
}
void Renderer::setRenderScale(float scale) {
    scale = std::clamp(scale, 0.25f, 2.0f);
    if (scale != m_impl->renderScale) {
        m_impl->renderScale = scale;
        m_impl->needsResize = true;
    }
}
float Renderer::renderScale() const { return m_impl->renderScale; }
uint32_t Renderer::renderWidth() const { return m_impl->sceneColor.extent.width; }
uint32_t Renderer::renderHeight() const { return m_impl->sceneColor.extent.height; }
void Renderer::setViewport(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) width = height = 0;
    if (width != m_impl->viewportSize.width || height != m_impl->viewportSize.height) {
        m_impl->viewportSize = {width, height};
        m_impl->needsSceneResize = true;
    }
}
uint64_t Renderer::sceneTexture() const { return reinterpret_cast<uint64_t>(m_impl->guiSceneTexture); }
void Renderer::setVsync(bool vsync) {
    if (vsync != m_impl->swapchain->vsync()) {
        m_impl->swapchain->setVsync(vsync);
        m_impl->needsResize = true;
    }
}
bool Renderer::vsync() const { return m_impl->swapchain->vsync(); }
float Renderer::aspectRatio() const {
    const VkExtent2D e = m_impl->outputExtent();
    return e.height ? static_cast<float>(e.width) / static_cast<float>(e.height) : 1.0f;
}
const char* Renderer::gpuName() const { return m_impl->context->gpuName(); }
uint32_t Renderer::drawCount() const { return m_impl->lastDrawCount; }
bool Renderer::beginFrame() { return m_impl->beginFrame(); }
void Renderer::endFrame() { m_impl->endFrame(); }
void Renderer::onResize() { m_impl->needsResize = true; }
void Renderer::waitIdle() { vkDeviceWaitIdle(m_impl->context->device()); }
void Renderer::initGui() { m_impl->initGui(); }
void Renderer::shutdownGui() { m_impl->shutdownGui(); }
void Renderer::newGuiFrame() { ImGui_ImplVulkan_NewFrame(); }

} // namespace shockwave
