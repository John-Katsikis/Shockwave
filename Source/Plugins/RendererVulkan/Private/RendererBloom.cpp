// Bloom, the way Call of Duty: Advanced Warfare does it (Jimenez, "Next Generation Post
// Processing in Call of Duty", SIGGRAPH 2014): no brightness threshold. The HDR scene is
// filtered into a chain of ever smaller levels (a 13-tap filter, with a Karis average on
// the first step so single bright pixels don't flicker), then back up, each level adding a
// small tent blur of the one below it. The tonemap pass mixes the top level into the
// scene by bloomIntensity, so only light far brighter than its surroundings visibly
// spreads. All of it is bloom.comp.
//
// The player's bloom_quality picks the chain (RenderTypes.h BloomQuality): where it starts
// and how many levels it has. Changing it makes a new chain beside the old one and retires
// the old one once the frames reading it finish; off frees it the same way.
#include "RendererImpl.h"

namespace shockwave {

namespace {

constexpr VkFormat kBloomFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr uint32_t kMaxBloomLevels = 6;
constexpr uint32_t kMinBloomSize = 8;   // stop halving before a level gets smaller than this

struct BloomTier {
    uint32_t divisor;   // the first level is the scene's size over this
    uint32_t levels;
};
BloomTier bloomTier(BloomQuality quality) {
    switch (quality) {
    case BloomQuality::Low: return {4, 4};
    case BloomQuality::Medium: return {2, 5};
    case BloomQuality::High: return {2, kMaxBloomLevels};
    case BloomQuality::Off: break;
    }
    return {0, 0};
}

// Must match the push constants in shaders/bloom.comp.
struct BloomPushConstants {
    glm::vec2 sourceTexel;   // 1 / the source's size
    glm::uvec2 targetSize;
    uint32_t mode;   // 0 first downsample (Karis average), 1 downsample, 2 upsample
};

}   // namespace

void VulkanRenderer::Impl::createBloomPipeline() {
    VkDevice device = context->device();
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk::check(vkCreateSampler(device, &samplerInfo, nullptr, &bloomSampler), "vkCreateSampler");

    // Each step reads one image and writes the next.
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[0].pImmutableSamplers = &bloomSampler;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    vk::check(
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &bloomSetLayout), "vkCreateDescriptorSetLayout");

    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BloomPushConstants)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &bloomSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &range;
    vk::check(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &bloomLayout), "vkCreatePipelineLayout");

    VkShaderModule module = vk::loadShaderModule(device, shaderPath("bloom.comp"));
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = bloomLayout;
    const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &bloomPipeline);
    vkDestroyShaderModule(device, module, nullptr);
    vk::check(result, "vkCreateComputePipelines");
}

void VulkanRenderer::Impl::destroyBloomPipeline() {
    VkDevice device = context->device();
    vkDestroyPipeline(device, bloomPipeline, nullptr);
    vkDestroyPipelineLayout(device, bloomLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, bloomSetLayout, nullptr);
    vkDestroySampler(device, bloomSampler, nullptr);
}

bool VulkanRenderer::Impl::createBloomTargets() {
    VkDevice device = context->device();
    const BloomTier tier = bloomTier(bloomQuality);
    if (!tier.levels) return true;
    // A slot no frame in flight reads: the old chain's stays put until it's retired.
    int free = -1;
    for (int i = 0; i < 2 && free < 0; ++i)
        if (!bloomSlotBusy[i] && !(bloom.image.image && bloomSlots[i] == bloom.slot)) free = i;
    if (free < 0) return false;

    BloomChain chain;
    chain.quality = bloomQuality;
    chain.slot = bloomSlots[free];
    VkExtent2D extent{
        std::max(1u, sceneColor.extent.width / tier.divisor), std::max(1u, sceneColor.extent.height / tier.divisor)};
    while (chain.extents.size() < tier.levels &&
           (chain.extents.empty() || std::min(extent.width, extent.height) >= kMinBloomSize)) {
        chain.extents.push_back(extent);
        extent = {std::max(1u, extent.width / 2), std::max(1u, extent.height / 2)};
    }
    const auto levels = static_cast<uint32_t>(chain.extents.size());
    chain.image = createImage(
        chain.extents[0], kBloomFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, levels, true);
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = chain.image.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kBloomFormat;
    for (uint32_t level = 0; level < levels; ++level) {
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        vk::check(vkCreateImageView(device, &viewInfo, nullptr, &view), "vkCreateImageView");
        chain.levelViews.push_back(view);
    }

    // Each chain has its own descriptors, so retiring it frees them with it.
    const uint32_t maxSets = 2 * kMaxBloomLevels;
    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxSets},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maxSets},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = maxSets;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    vk::check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &chain.pool), "vkCreateDescriptorPool");

    // The chain stays in GENERAL: every level is read by one step and written by another.
    auto set = [&](VkImageView source, VkImageLayout sourceLayout, VkImageView target) {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = chain.pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &bloomSetLayout;
        VkDescriptorSet result = VK_NULL_HANDLE;
        vk::check(vkAllocateDescriptorSets(device, &allocInfo, &result), "vkAllocateDescriptorSets");
        const VkDescriptorImageInfo images[2] = {
            {VK_NULL_HANDLE, source, sourceLayout},
            {VK_NULL_HANDLE, target, VK_IMAGE_LAYOUT_GENERAL},
        };
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
    for (uint32_t level = 0; level < levels; ++level) {
        chain.downSets.push_back(
            level == 0 ? set(sceneColor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, chain.levelViews[0])
                       : set(chain.levelViews[level - 1], VK_IMAGE_LAYOUT_GENERAL, chain.levelViews[level]));
    }
    for (uint32_t level = 0; level + 1 < levels; ++level)
        chain.upSets.push_back(set(chain.levelViews[level + 1], VK_IMAGE_LAYOUT_GENERAL, chain.levelViews[level]));

    // The tonemap pass reads the top level through the bindless table. The slot is one no
    // frame in flight uses, which the table allows (update unused while pending).
    {
        std::lock_guard lock(descriptorMutex);
        VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, chain.levelViews[0], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = bindlessSet;
        write.dstBinding = 0;
        write.dstArrayElement = chain.slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
    if (bloom.image.image) {
        // Still read by frames in flight: free it once they finish.
        const int slot = bloom.slot == bloomSlots[0] ? 0 : 1;
        bloomSlotBusy[slot] = true;
        retire([this, old = bloom, slot]() mutable {
            destroyBloomChain(old);
            bloomSlotBusy[slot] = false;
        });
    }
    bloom = std::move(chain);
    return true;
}

void VulkanRenderer::Impl::destroyBloomChain(BloomChain& chain) {
    VkDevice device = context->device();
    if (chain.pool) vkDestroyDescriptorPool(device, chain.pool, nullptr);
    for (VkImageView view : chain.levelViews) vkDestroyImageView(device, view, nullptr);
    destroyImage(chain.image);
    chain = {};
}

// Only called with the GPU idle.
void VulkanRenderer::Impl::destroyBloomTargets() { destroyBloomChain(bloom); }

void VulkanRenderer::Impl::updateBloomTargets() {
    const BloomQuality have = bloom.image.image ? bloom.quality : BloomQuality::Off;
    if (have == bloomQuality) return;
    if (bloomQuality == BloomQuality::Off) {
        const int slot = bloom.slot == bloomSlots[0] ? 0 : 1;
        bloomSlotBusy[slot] = true;
        retire([this, old = bloom, slot]() mutable {
            destroyBloomChain(old);
            bloomSlotBusy[slot] = false;
        });
        bloom = {};
        return;
    }
    createBloomTargets();   // false: both slots in flight; the next frame tries again
}

float VulkanRenderer::Impl::bloomIntensity() const {
    // Debug views show raw values, like the tonemap pass.
    const bool lit =
        debugView == DebugView::Lit || debugView == DebugView::Lighting || debugView == DebugView::ShadowCascades;
    return lit && bloom.image.image ? std::clamp(authoredBloom(), 0.0f, 1.0f) * bloomScale : 0.0f;
}

void VulkanRenderer::Impl::recordBloom(VkCommandBuffer cmd) {
    auto barrier = [&](VkImageLayout from, VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage) {
        transitionImage(
            cmd, bloom.image.image, VK_IMAGE_ASPECT_COLOR_BIT, from, VK_IMAGE_LAYOUT_GENERAL, srcStage,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, dstStage,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    };
    // Last frame's contents aren't needed; wait for last frame's tonemap to finish reading.
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bloomPipeline);
    auto dispatch = [&](VkDescriptorSet set, VkExtent2D source, uint32_t level, uint32_t mode) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bloomLayout, 0, 1, &set, 0, nullptr);
        const VkExtent2D target = bloom.extents[level];
        // The 13 taps reach two source texels either way, which covers a halving. Starting
        // at a quarter (low quality), spread them over the larger footprint instead.
        const float spread = std::max(1.0f, static_cast<float>(source.width) / static_cast<float>(target.width) * 0.5f);
        BloomPushConstants push{
            glm::vec2(spread / static_cast<float>(source.width), spread / static_cast<float>(source.height)),
            glm::uvec2(target.width, target.height), mode};
        vkCmdPushConstants(cmd, bloomLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (target.width + 7) / 8, (target.height + 7) / 8, 1);
        barrier(
            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    };
    const auto levels = static_cast<uint32_t>(bloom.extents.size());
    for (uint32_t level = 0; level < levels; ++level)
        dispatch(
            bloom.downSets[level], level == 0 ? sceneColor.extent : bloom.extents[level - 1], level,
            level == 0 ? 0 : 1);
    for (uint32_t level = levels - 1; level-- > 0;) dispatch(bloom.upSets[level], bloom.extents[level + 1], level, 2);
    barrier(VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
}

}   // namespace shockwave
