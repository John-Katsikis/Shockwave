#include "RendererImpl.h"

namespace shockwave {

VulkanRenderer::Impl::Impl(Window& w, JobSystem& j, bool vsync) : window(w), jobs(j) {
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
    createFrames();
    createRenderFinishedSemaphores();
    createBindless();
    createPipelines();

    const uint8_t white[4] = {255, 255, 255, 255};
    createTexture({0}, 1, 1, white, false);   // slot 0: the default texture
    sceneSlot = context->maxBindlessTextures() - 1;
    cascadeSlot = context->maxBindlessTextures() - 2;
    localSlot = context->maxBindlessTextures() - 3;
    createRenderTargets();
    createShadowMaps();
}

VulkanRenderer::Impl::~Impl() {
    VkDevice device = context->device();
    VmaAllocator allocator = context->allocator();
    vkDeviceWaitIdle(device);
    if (guiEnabled) shutdownGui();
    for (uint32_t i = 0; i < kMaxMeshes; ++i) {
        if (!meshReady[i].load()) continue;
        vmaDestroyBuffer(allocator, meshes[i].vertices.buffer, meshes[i].vertices.allocation);
        vmaDestroyBuffer(allocator, meshes[i].indices.buffer, meshes[i].indices.allocation);
    }
    for (uint32_t i = 0; i < textureCapacity(); ++i) destroyImage(textures[i]);
    for (TextureSwap& swap : textureSwaps) destroyImage(swap.image);
    vkDestroyCommandPool(device, uploadPool, nullptr);
    destroySkinning();
    destroyImage(sceneColor);
    destroyImage(sceneDepth);
    destroyDisplayUiView();
    destroyImage(displayColor);
    destroyShadowMaps();
    destroyUiPipeline();
    for (Frame& f : frames) destroyUiBuffers(f);
    vkDestroyPipeline(device, shadowPipeline, nullptr);
    vkDestroyPipelineLayout(device, shadowLayout, nullptr);
    vkDestroySampler(device, shadowSampler, nullptr);
    vkDestroyPipeline(device, tonemapPipeline, nullptr);
    vkDestroyPipelineLayout(device, tonemapLayout, nullptr);
    for (auto pipeline : mirroredPipelines) vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipeline(device, meshPipeline, nullptr);
    vkDestroyPipeline(device, meshDoubleSidedPipeline, nullptr);
    vkDestroyPipeline(device, meshBlendPipeline, nullptr);
    vkDestroyPipeline(device, meshBlendDoubleSidedPipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyPipeline(device, gridPipeline, nullptr);
    vkDestroyPipelineLayout(device, gridLayout, nullptr);
    vkDestroySampler(device, linearSampler, nullptr);
    vkDestroyDescriptorPool(device, bindlessPool, nullptr);
    vkDestroyDescriptorSetLayout(device, bindlessLayout, nullptr);
    destroyRenderFinishedSemaphores();
    vkDestroySemaphore(device, frameTimeline, nullptr);
    for (Frame& f : frames) {
        vmaDestroyBuffer(allocator, f.drawBuffer.buffer, f.drawBuffer.allocation);
        vmaDestroyBuffer(allocator, f.frameBuffer.buffer, f.frameBuffer.allocation);
        vkDestroySemaphore(device, f.imageAcquired, nullptr);
        vkDestroyQueryPool(device, f.timestamps, nullptr);
        vkDestroyCommandPool(device, f.pool, nullptr);
    }
    swapchain.reset();
    context.reset();
}

void VulkanRenderer::Impl::checkBlitSupport() {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(context->physicalDevice(), swapchain->format(), &props);
    if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        throw std::runtime_error("The swapchain format can't be a blit target on this GPU");
    }
}

vk::Image VulkanRenderer::Impl::createImage(
    VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t mipLevels,
    bool dedicated, VkImageCreateFlags flags) {
    vk::Image image;
    image.extent = extent;
    image.format = format;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = flags;
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
    vk::check(
        vmaCreateImage(context->allocator(), &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr),
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

void VulkanRenderer::Impl::destroyImage(vk::Image& image) {
    if (image.view) vkDestroyImageView(context->device(), image.view, nullptr);
    if (image.image) vmaDestroyImage(context->allocator(), image.image, image.allocation);
    image = {};
}

void VulkanRenderer::Impl::createRenderTargets() {
    const VkExtent2D out = outputExtent();
    const VkExtent2D extent{
        std::max(1u, static_cast<uint32_t>(std::lround(out.width * renderScale))),
        std::max(1u, static_cast<uint32_t>(std::lround(out.height * renderScale))),
    };
    sceneColor = createImage(
        extent, kSceneColorFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, 1, true);
    sceneDepth = createImage(
        extent, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT, 1, true);
    displayColor = createImage(
        out, kDisplayFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, 1, true, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
    createDisplayUiView();
    sceneColorState = sceneDepthState = displayState = {};   // new images start undefined

    // The tonemap pass reads the scene through the bindless table like any texture.
    std::lock_guard lock(descriptorMutex);
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = sceneColor.view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = bindlessSet;
    write.dstBinding = 0;
    write.dstArrayElement = sceneSlot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(context->device(), 1, &write, 0, nullptr);
    registerGuiSceneTexture();
}

void VulkanRenderer::Impl::registerGuiSceneTexture() {
    if (!guiEnabled) return;
    if (guiSceneTexture) ImGui_ImplVulkan_RemoveTexture(guiSceneTexture);
    guiSceneTexture = ImGui_ImplVulkan_AddTexture(displayColor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanRenderer::Impl::recreateSceneTargets() {
    vkDeviceWaitIdle(context->device());
    destroyImage(sceneColor);
    destroyImage(sceneDepth);
    destroyDisplayUiView();
    destroyImage(displayColor);
    createRenderTargets();
    needsSceneResize = false;
}

}   // namespace shockwave
