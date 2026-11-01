// Occlusion culling (G12, docs/Drawing.md, Renderer/Culling.h). On frames that already
// draw the surface prepass, one pass between it and the scene builds a depth pyramid from
// the prepass's depth (hzb.comp, the farthest depth of each texel's children) and tests
// each opaque draw the camera sees against it (cull.comp), writing that draw's indirect
// command with one instance or none. The scene pass then draws them with
// vkCmdDrawIndexedIndirect, a group of draws a call where the device has multi-draw
// indirect. Nothing waits on the GPU: the counts of tested and occluded draws are read
// once the frame's slot comes round again.
//
// It's conservative by construction (docs/GraphicsImplementation.md 16.2): a draw is
// skipped only when every depth texel under its bounds is nearer than its nearest corner,
// so none of its fragments could have passed the scene's EQUAL depth test anyway. Shadow
// maps, glass, skin, skinned meshes, blended surfaces and reflection captures are never
// culled by it.
#include "RendererImpl.h"

#include <bit>
#include <cmath>

namespace shockwave {

namespace {

constexpr VkFormat kHzbFormat = VK_FORMAT_R32_SFLOAT;
constexpr uint32_t kCounterBytes = 2 * sizeof(uint32_t);

// Must match the push constants in shaders/hzb.comp.
struct HzbPushConstants {
    glm::uvec2 sourceSize;
    glm::uvec2 targetSize;
};
// Must match the push constants in shaders/cull.comp.
struct CullPushConstants {
    glm::mat4 viewProjection;
    VkDeviceAddress draws;
    VkDeviceAddress commands;
    VkDeviceAddress counters;
    glm::uvec2 depthSize;
    uint32_t count;
    uint32_t levels;
    glm::uvec2 hzbSize;   // level 0's texels; level L is this >> L, at least 1
};
static_assert(sizeof(CullPushConstants) <= 128, "push constants must fit the guaranteed 128 bytes");

uint64_t imageBytes(VmaAllocator allocator, const vk::Image& image) {
    if (!image.image) return 0;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(allocator, image.allocation, &info);
    return info.size;
}

VkPipeline computePipeline(VkDevice device, VkPipelineLayout layout, const std::string& path) {
    VkShaderModule module = vk::loadShaderModule(device, path);
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateComputePipelines(device, vk::pipelineCache(), 1, &info, nullptr, &pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    vk::check(result, "vkCreateComputePipelines");
    return pipeline;
}

}   // namespace

void VulkanRenderer::Impl::createCullingPipelines() {
    VkDevice device = context->device();
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    vk::check(vkCreateSampler(device, &samplerInfo, nullptr, &hzbSampler), "vkCreateSampler");

    // One layout for both: what's read (a sampler) and, for the pyramid, the level written.
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[0].pImmutableSamplers = &hzbSampler;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    vk::check(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &cullSetLayout), "vkCreateDescriptorSetLayout");

    auto layout = [&](uint32_t pushBytes) {
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1;
        info.pSetLayouts = &cullSetLayout;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;
        VkPipelineLayout result = VK_NULL_HANDLE;
        vk::check(vkCreatePipelineLayout(device, &info, nullptr, &result), "vkCreatePipelineLayout");
        return result;
    };
    hzbLayout = layout(sizeof(HzbPushConstants));
    cullLayout = layout(sizeof(CullPushConstants));
    hzbPipeline = computePipeline(device, hzbLayout, shaderPath("hzb.comp"));
    cullPipeline = computePipeline(device, cullLayout, shaderPath("cull.comp"));
}

void VulkanRenderer::Impl::destroyCullingPipelines() {
    VkDevice device = context->device();
    VmaAllocator allocator = context->allocator();
    destroyHzb(hzb);
    for (Frame& f : frames) {
        for (vk::Buffer* b : {&f.cullInput, &f.cullCommands, &f.cullCounters})
            if (b->buffer) vmaDestroyBuffer(allocator, b->buffer, b->allocation);
        f.cullInput = f.cullCommands = f.cullCounters = {};
        f.cullCapacity = 0;
        f.cullData = nullptr;
        f.cullCounterData = nullptr;
        f.cullCountersPending = false;
    }
    vkDestroyPipeline(device, hzbPipeline, nullptr);
    vkDestroyPipeline(device, cullPipeline, nullptr);
    vkDestroyPipelineLayout(device, hzbLayout, nullptr);
    vkDestroyPipelineLayout(device, cullLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, cullSetLayout, nullptr);
    vkDestroySampler(device, hzbSampler, nullptr);
}

void VulkanRenderer::Impl::destroyHzb(Hzb& pyramid) {
    VkDevice device = context->device();
    if (pyramid.pool) vkDestroyDescriptorPool(device, pyramid.pool, nullptr);
    for (VkImageView view : pyramid.levelViews) vkDestroyImageView(device, view, nullptr);
    if (pyramid.allView) vkDestroyImageView(device, pyramid.allView, nullptr);
    destroyImage(pyramid.image);
    pyramid = {};
}

void VulkanRenderer::Impl::updateHzb(bool want) {
    // Level 0 halves the depth, rounding up, then is padded to a power of two a side: Vulkan's
    // mip levels round down, so only power-of-two sizes halve exactly at every level, as the
    // reduction (Renderer/Culling.h) needs. Padding holds 1.0, the nearest depth, which never
    // makes anything look hidden; the screen's edge is clamped to before it's read.
    auto pow2 = [](uint32_t v) { return uint32_t(std::bit_ceil(std::max(v, 1u))); };
    const VkExtent2D extent{pow2((sceneDepth.extent.width + 1) / 2), pow2((sceneDepth.extent.height + 1) / 2)};
    const bool current = hzb.image.image && hzb.depthView == sceneDepth.view && hzb.extent.width == extent.width &&
                         hzb.extent.height == extent.height;
    if (want == static_cast<bool>(hzb.image.image) && (!want || current)) return;
    if (hzb.image.image) {
        // Frames in flight may still read it: free it once they finish.
        retire([this, old = hzb]() mutable { destroyHzb(old); });
        hzb = {};
    }
    if (!want) return;

    VkDevice device = context->device();
    Hzb pyramid;
    pyramid.extent = extent;
    pyramid.depthView = sceneDepth.view;
    // Down to 1 x 1: as many levels as Vulkan allows for this size.
    pyramid.levels = uint32_t(std::bit_width(std::max(extent.width, extent.height)));
    pyramid.image = createImage(
        extent, kHzbFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
        pyramid.levels);
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = pyramid.image.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kHzbFormat;
    for (uint32_t level = 0; level < pyramid.levels; ++level) {
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        vk::check(vkCreateImageView(device, &viewInfo, nullptr, &view), "vkCreateImageView");
        pyramid.levelViews.push_back(view);
    }
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, pyramid.levels, 0, 1};
    vk::check(vkCreateImageView(device, &viewInfo, nullptr, &pyramid.allView), "vkCreateImageView");

    const uint32_t sets = pyramid.levels + 1;
    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sets}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = sets;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    vk::check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pyramid.pool), "vkCreateDescriptorPool");
    auto set = [&](VkImageView source, VkImageLayout sourceLayout, VkImageView target) {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = pyramid.pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &cullSetLayout;
        VkDescriptorSet result = VK_NULL_HANDLE;
        vk::check(vkAllocateDescriptorSets(device, &allocInfo, &result), "vkAllocateDescriptorSets");
        const VkDescriptorImageInfo images[2] = {
            {VK_NULL_HANDLE, source, sourceLayout}, {VK_NULL_HANDLE, target, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = result;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType =
                i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
        return result;
    };
    for (uint32_t level = 0; level < pyramid.levels; ++level)
        pyramid.levelSets.push_back(
            level == 0 ? set(sceneDepth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, pyramid.levelViews[0])
                       : set(pyramid.levelViews[level - 1], VK_IMAGE_LAYOUT_GENERAL, pyramid.levelViews[level]));
    // cull.comp reads every level; the storage binding is unused there, but must be valid.
    pyramid.cullSet = set(pyramid.allView, VK_IMAGE_LAYOUT_GENERAL, pyramid.levelViews[0]);
    hzb = std::move(pyramid);
}

void VulkanRenderer::Impl::prepareOcclusion(Frame& f) {
    VmaAllocator allocator = context->allocator();
    // This slot's last frame is finished (beginFrame waited): its counts can be read.
    if (f.cullCountersPending) {
        vk::check(vmaInvalidateAllocation(allocator, f.cullCounters.allocation, 0, kCounterBytes), "vmaInvalidate");
        lastTested = f.cullCounterData[0];
        lastOccluded = f.cullCounterData[1];
        f.cullCountersPending = false;
    }

    occlusionThisFrame = false;
    cullCount = 0;
    if (drawSettings.occlusion == OcclusionCulling::Off)
        occlusionReason = "occlusion_culling off";
    else if (!prepassActive())
        occlusionReason = "no depth prepass this frame (ambient occlusion, TAA and decals are off)";
    else if (!context->drawIndirectFirstInstance())
        occlusionReason = "the GPU can't start indirect draws at an instance (drawIndirectFirstInstance)";
    else if (captureRequested.load(std::memory_order_relaxed))
        occlusionReason = "a reflection capture frame draws everything";
    else
        occlusionReason = "";
    const bool wanted = occlusionReason[0] == '\0';
    updateHzb(wanted);
    if (!wanted) {
        lastTested = lastOccluded = 0;
        return;
    }

    // The candidates: opaque draws in view, rigid (a skinned mesh's bounds are its bind
    // pose), not glass. In table order, which is the scene pass's order.
    uint32_t count = 0;
    for (const DrawCommand& draw : gatheredDraws) count += draw.visible && !draw.blend && draw.skin == kNoSkin;
    if (count == 0) {
        occlusionReason = "no opaque rigid draws in view";
        return;
    }
    if (count > f.cullCapacity) {
        for (vk::Buffer* b : {&f.cullInput, &f.cullCommands})
            if (b->buffer) vmaDestroyBuffer(allocator, b->buffer, b->allocation);
        const uint32_t capacity = std::max(count + count / 2, 256u);
        VmaAllocationInfo info{};
        f.cullInput = createBuffer(
            VkDeviceSize(capacity) * sizeof(CullDraw),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, &info);
        f.cullData = static_cast<CullDraw*>(info.pMappedData);
        f.cullInputAddress = bufferAddress(f.cullInput.buffer);
        f.cullCommands = createBuffer(
            VkDeviceSize(capacity) * sizeof(VkDrawIndexedIndirectCommand),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            0);
        f.cullCommandsAddress = bufferAddress(f.cullCommands.buffer);
        f.cullCommandsState = {};
        f.cullCapacity = capacity;
    }
    if (!f.cullCounters.buffer) {
        VmaAllocationInfo info{};
        f.cullCounters = createBuffer(
            kCounterBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, &info);
        f.cullCounterData = static_cast<uint32_t*>(info.pMappedData);
        f.cullCountersAddress = bufferAddress(f.cullCounters.buffer);
    }
    for (DrawCommand& draw : gatheredDraws) {
        draw.cullSlot = kNoCull;
        if (!draw.visible || draw.blend || draw.skin != kNoSkin) continue;
        draw.cullSlot = cullCount;
        f.cullData[cullCount++] = {draw.bounds, draw.drawIndex, draw.indexCount, draw.firstIndex, 0};
    }
    f.cullCounterData[0] = f.cullCounterData[1] = 0;
    vk::check(
        vmaFlushAllocation(allocator, f.cullInput.allocation, 0, VkDeviceSize(cullCount) * sizeof(CullDraw)),
        "vmaFlushAllocation");
    vk::check(vmaFlushAllocation(allocator, f.cullCounters.allocation, 0, kCounterBytes), "vmaFlushAllocation");
    f.cullCountersPending = true;
    occlusionThisFrame = true;
}

void VulkanRenderer::Impl::recordOcclusion(VkCommandBuffer cmd, Frame& f) {
    auto barrier = [&](VkImageLayout from, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess) {
        transitionImage(
            cmd, hzb.image.image, VK_IMAGE_ASPECT_COLOR_BIT, from, VK_IMAGE_LAYOUT_GENERAL, srcStage, srcAccess,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    };
    // Last frame's pyramid isn't needed; wait for last frame's culling to finish reading it.
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hzbPipeline);
    VkExtent2D source = sceneDepth.extent;
    VkExtent2D target = hzb.extent;
    for (uint32_t level = 0; level < hzb.levels; ++level) {
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hzbLayout, 0, 1, &hzb.levelSets[level], 0, nullptr);
        const HzbPushConstants push{{source.width, source.height}, {target.width, target.height}};
        vkCmdPushConstants(cmd, hzbLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (target.width + 7) / 8, (target.height + 7) / 8, 1);
        barrier(VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        source = target;
        target = {(target.width + 1) / 2, (target.height + 1) / 2};
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullLayout, 0, 1, &hzb.cullSet, 0, nullptr);
    CullPushConstants push{};
    push.viewProjection = viewProjection;   // jittered, as the prepass drew
    push.draws = f.cullInputAddress;
    push.commands = f.cullCommandsAddress;
    push.counters = f.cullCountersAddress;
    push.depthSize = {sceneDepth.extent.width, sceneDepth.extent.height};
    push.count = cullCount;
    push.levels = hzb.levels;
    push.hzbSize = {hzb.extent.width, hzb.extent.height};
    vkCmdPushConstants(cmd, cullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (cullCount + 63) / 64, 1, 1);
}

DrawStats VulkanRenderer::Impl::finishDrawStats(Frame&) {
    DrawStats stats;
    stats.occlusionActive = occlusionThisFrame;
    stats.occlusionReason = occlusionReason;
    stats.multiDrawIndirect = occlusionThisFrame && context->multiDrawIndirect();
    stats.tested = lastTested;
    stats.occluded = lastOccluded;
    stats.hzbBytes = imageBytes(context->allocator(), hzb.image);
    stats.pipelineCacheFile = pipelineCacheFile.string();
    stats.pipelineCacheBytes = pipelineCacheLoadedBytes;
    stats.pipelineCacheState = pipelineCacheState;
    return stats;
}

}   // namespace shockwave
