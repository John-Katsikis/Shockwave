#include "RendererImpl.h"

namespace shockwave {

void Renderer::Impl::recordScreenshotCopy(VkCommandBuffer cmd) {
    screenshotExtent = displayColor.extent;
    VmaAllocationInfo info{};
    screenshotBuffer = createBuffer(
        VkDeviceSize(screenshotExtent.width) * screenshotExtent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, &info);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {screenshotExtent.width, screenshotExtent.height, 1};
    vkCmdCopyImageToBuffer(
        cmd, displayColor.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, screenshotBuffer.buffer, 1, &region);
    // Make the copy visible to the CPU once the frame's timeline value is reached.
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void Renderer::Impl::finishScreenshot() {
    screenshotRequested = false;
    if (!screenshotBuffer.buffer) {
        screenshotMessage = "the frame was skipped";
        return;
    }
    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &frameTimeline;
    waitInfo.pValues = &frameNumber;
    vk::check(vkWaitSemaphores(context->device(), &waitInfo, UINT64_MAX), "vkWaitSemaphores");
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(context->allocator(), screenshotBuffer.allocation, &info);
    vmaInvalidateAllocation(context->allocator(), screenshotBuffer.allocation, 0, VK_WHOLE_SIZE);
    // The display image is sRGB-encoded RGBA8: exactly what a PNG stores.
    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        static_cast<int>(screenshotExtent.width), static_cast<int>(screenshotExtent.height), SDL_PIXELFORMAT_RGBA32,
        info.pMappedData, static_cast<int>(screenshotExtent.width * 4));
    screenshotMessage.clear();
    if (!surface || !SDL_SavePNG(surface, screenshotPath.c_str())) {
        screenshotMessage = std::string("can't save ") + screenshotPath + ": " + SDL_GetError();
    }
    if (surface) SDL_DestroySurface(surface);
    vmaDestroyBuffer(context->allocator(), screenshotBuffer.buffer, screenshotBuffer.allocation);
    screenshotBuffer = {};
}

}   // namespace shockwave
