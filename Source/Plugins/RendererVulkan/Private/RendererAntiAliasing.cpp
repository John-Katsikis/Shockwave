// Anti-aliasing after tone mapping (G01). With FXAA on, the tonemap pass writes aaTarget
// instead of the display image, and Shaders/fxaa.frag writes the display image from it,
// before game UI, so text and crosshairs are never blurred. With it off, there's no image,
// no pass and no descriptor: the tonemap pass writes the display image as before.
//
// aaTarget has the display image's sRGB format, so tone mapping encodes exactly as it
// would into the display image. FXAA reads it through a UNORM view (display-encoded
// values, what its luma thresholds are tuned for) and writes the display image through
// its UNORM view too, so the result is never encoded twice.
//
// Turning it on or off happens at a frame boundary: the new image is made (or none), and
// the old one is retired, freed once the frames using it have finished. Nothing waits for
// the GPU.
#include "RendererImpl.h"

namespace shockwave {

namespace {

// Must match the push constants in shaders/fxaa.frag.
struct FxaaPushConstants {
    glm::vec2 texel;
};

}   // namespace

void VulkanRenderer::Impl::retire(std::function<void()> destroy) {
    // Frames 0..frameNumber-1 are submitted; the last signals frameNumber when it's done.
    retired.push_back({frameNumber, std::move(destroy)});
}

void VulkanRenderer::Impl::collectRetired(bool all) {
    if (retired.empty()) return;
    uint64_t done = UINT64_MAX;
    if (!all)
        vk::check(vkGetSemaphoreCounterValue(context->device(), frameTimeline, &done), "vkGetSemaphoreCounterValue");
    std::vector<Retired> keep;
    std::vector<Retired> now;
    for (Retired& r : retired) (r.afterFrame <= done ? now : keep).push_back(std::move(r));
    retired = std::move(keep);
    for (Retired& r : now) r.destroy();
}

void VulkanRenderer::Impl::createAntiAliasingPipeline() {
    VkDevice device = context->device();
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk::check(vkCreateSampler(device, &samplerInfo, nullptr, &aaSampler), "vkCreateSampler");

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding.pImmutableSamplers = &aaSampler;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    vk::check(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &aaSetLayout), "vkCreateDescriptorSetLayout");

    VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FxaaPushConstants)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &aaSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &range;
    vk::check(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &fxaaLayout), "vkCreatePipelineLayout");

    // Small and shared, so it's made once, whether or not FXAA is on.
    vk::GraphicsPipelineDesc desc;
    desc.vertexShader = shaderPath("tonemap.vert");   // one triangle over the whole target
    desc.fragmentShader = shaderPath("fxaa.frag");
    desc.colorFormat = kUiTargetFormat;   // the display image's UNORM view
    desc.layout = fxaaLayout;
    desc.cullMode = VK_CULL_MODE_NONE;
    fxaaPipeline = vk::createGraphicsPipeline(device, desc);
}

void VulkanRenderer::Impl::destroyAntiAliasingPipeline() {
    VkDevice device = context->device();
    vkDestroyPipeline(device, fxaaPipeline, nullptr);
    vkDestroyPipelineLayout(device, fxaaLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, aaSetLayout, nullptr);
    vkDestroySampler(device, aaSampler, nullptr);
}

void VulkanRenderer::Impl::createAaTarget() {
    VkDevice device = context->device();
    AaTarget target;
    target.image = createImage(
        displayColor.extent, kDisplayFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, 1, true, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = target.image.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kUiTargetFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk::check(vkCreateImageView(device, &viewInfo, nullptr, &target.encodedView), "vkCreateImageView");

    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    vk::check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &target.pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = target.pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &aaSetLayout;
    vk::check(vkAllocateDescriptorSets(device, &allocInfo, &target.set), "vkAllocateDescriptorSets");
    VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, target.encodedView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = target.set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    aaTarget = target;
    aaState = {};   // a new image starts undefined
}

void VulkanRenderer::Impl::destroyAaTarget(AaTarget& target) {
    VkDevice device = context->device();
    if (target.pool) vkDestroyDescriptorPool(device, target.pool, nullptr);
    if (target.encodedView) vkDestroyImageView(device, target.encodedView, nullptr);
    destroyImage(target.image);
    target = {};
}

void VulkanRenderer::Impl::updateAaTarget() {
    const bool want = antiAliasing == AntiAliasing::Fxaa;
    if (want == (aaTarget.image.image != VK_NULL_HANDLE)) return;
    if (aaTarget.image.image) {
        retire([this, old = aaTarget]() mutable { destroyAaTarget(old); });
        aaTarget = {};
    } else {
        createAaTarget();
    }
}

bool VulkanRenderer::Impl::fxaaActive() const {
    // Debug views show raw values, like the tonemap pass.
    const bool lit =
        debugView == DebugView::Lit || debugView == DebugView::Lighting || debugView == DebugView::ShadowCascades;
    return lit && aaTarget.image.image;
}

void VulkanRenderer::Impl::recordFxaa(VkCommandBuffer cmd) {
    VkRenderingAttachmentInfo target{};
    target.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    target.imageView = displayUiView;
    target.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    target.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // every pixel is written
    target.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, displayColor.extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &target;
    vkCmdBeginRendering(cmd, &rendering);
    const VkExtent2D extent = displayColor.extent;
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fxaaPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fxaaLayout, 0, 1, &aaTarget.set, 0, nullptr);
    const FxaaPushConstants push{
        glm::vec2(1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height))};
    vkCmdPushConstants(cmd, fxaaLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

}   // namespace shockwave
