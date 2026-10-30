// Local reflection probes (G07, Renderer/ReflectionProbes.h, docs/Lighting.md): cube textures
// in a bindless table of their own, the probe table each frame, which probes a draw reads,
// and the linear HDR scene capture the engine builds probes from.
#include "RendererImpl.h"

#include <glm/gtc/packing.hpp>

namespace shockwave {

// ---- Cube textures ----

vk::Image VulkanRenderer::Impl::uploadCube(const TextureData& data) {
    if (!data.valid() || data.shape != TextureShape::Cube) throw std::runtime_error("cube texture data is incomplete");
    if (!supportsTextureFormat(data.format))
        throw std::runtime_error(std::string("this GPU can't sample ") + textureFormatName(data.format));
    const auto levels = static_cast<uint32_t>(data.levels.size());
    const VkFormat format = vulkanFormat(data.format, data.srgb);
    vk::Image image;
    image.extent = {data.width, data.height};
    image.format = format;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {data.width, data.height, 1};
    imageInfo.mipLevels = levels;
    imageInfo.arrayLayers = 6;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    vk::check(
        vmaCreateImage(context->allocator(), &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr),
        "vmaCreateImage");
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 6};
    vk::check(vkCreateImageView(context->device(), &viewInfo, nullptr, &image.view), "vkCreateImageView");

    // Each level is its six faces in a row: one copy per face per level.
    std::vector<VkBufferImageCopy> copies;
    for (uint32_t level = 0; level < levels; ++level) {
        const TextureData::Level& l = data.levels[level];
        const size_t face = l.size / 6;
        for (uint32_t f = 0; f < 6; ++f) {
            VkBufferImageCopy copy{};
            copy.bufferOffset = l.offset + face * f;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, f, 1};
            copy.imageExtent = {l.width, l.height, 1};
            copies.push_back(copy);
        }
    }
    vk::Buffer staging = createStaging(data.bytes.data(), data.bytes.size());
    immediateSubmit([&](VkCommandBuffer cmd) {
        transitionImage(
            cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, VK_REMAINING_MIP_LEVELS, 6);
        vkCmdCopyBufferToImage(
            cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()),
            copies.data());
        transitionImage(
            cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, VK_REMAINING_MIP_LEVELS,
            6);
    });
    vmaDestroyBuffer(context->allocator(), staging.buffer, staging.allocation);
    return image;
}

void VulkanRenderer::Impl::publishCube(uint32_t id, vk::Image& image) {
    uint32_t slot = kNoSlot;
    {
        std::lock_guard lock(textureSlotMutex);
        if (!freeCubeSlots.empty()) {
            slot = freeCubeSlots.back();
            freeCubeSlots.pop_back();
        }
    }
    if (slot == kNoSlot) {
        destroyImage(image);
        throw std::runtime_error(
            "createTexture: every cube slot is in use (" + std::to_string(kReflectionCubeSlots) + ")");
    }
    textures[id] = image;
    writeCubeDescriptor(slot, image.view);
    cubeSlots[id].store(slot, std::memory_order_release);
}

void VulkanRenderer::Impl::writeCubeDescriptor(uint32_t slot, VkImageView view) {
    std::lock_guard lock(descriptorMutex);
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = bindlessSet;
    write.dstBinding = 7;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(context->device(), 1, &write, 0, nullptr);
}

// ---- The probe table ----

void VulkanRenderer::Impl::setReflectionProbes(std::span<const ReflectionProbeDesc> probes) {
    auto next = std::make_shared<const std::vector<ReflectionProbeDesc>>(
        probes.begin(), probes.begin() + std::min<size_t>(probes.size(), kMaxReflectionProbes));
    std::atomic_store(&reflectionProbes, std::move(next));
}

uint32_t VulkanRenderer::Impl::pickReflections(const glm::vec4& bounds) const {
    // Off, or a capture frame (the cube being made mustn't see other probes): the global
    // environment only, exactly as without probes.
    if (!localReflections.load(std::memory_order_relaxed) || captureRequested.load(std::memory_order_relaxed)) return 0;
    const auto probes = std::atomic_load(&reflectionProbes);
    if (probes->empty()) return 0;
    const ReflectionSelection s = selectReflectionProbes(*probes, bounds);
    uint32_t packed = 0;
    for (uint32_t i = 0; i < s.count; ++i) packed |= (s.index[i] + 1u) << (16 * i);
    return packed;
}

uint32_t VulkanRenderer::Impl::writeReflectionProbes(Frame& f) {
    const auto probes = std::atomic_load(&reflectionProbes);
    auto* out = reinterpret_cast<ReflectionProbeData*>(f.frameData + kReflectionProbesOffset);
    const auto count = static_cast<uint32_t>(probes->size());
    for (uint32_t i = 0; i < count; ++i) {
        const ReflectionProbeDesc& p = (*probes)[i];
        ReflectionProbeData& d = out[i];
        d = {};
        d.worldToBox = glm::inverse(p.boxToWorld);
        d.halfExtents = glm::max(p.halfExtents, glm::vec3(1e-4f));
        d.blendDistance = std::max(p.blendDistance, 0.0f);
        d.capturePosition = p.capturePosition;
        const uint32_t slot = cubeSlot(p.cube.id);
        d.cube = slot == kNoSlot ? kNoCube : slot;
        d.priority = p.priority;
    }
    return count;
}

// ---- Scene capture ----

void VulkanRenderer::Impl::recordCaptureCopy(VkCommandBuffer cmd) {
    captureExtent = sceneColor.extent;
    VmaAllocationInfo info{};
    captureBuffer = createBuffer(
        VkDeviceSize(captureExtent.width) * captureExtent.height * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, &info);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {captureExtent.width, captureExtent.height, 1};
    vkCmdCopyImageToBuffer(
        cmd, sceneColor.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuffer.buffer, 1, &region);
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

void VulkanRenderer::Impl::finishCapture() {
    captureRequested = false;
    capture = {};
    if (!captureBuffer.buffer) {
        capture.error = "the frame was skipped";
        return;
    }
    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &frameTimeline;
    waitInfo.pValues = &frameNumber;
    vk::check(vkWaitSemaphores(context->device(), &waitInfo, UINT64_MAX), "vkWaitSemaphores");
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(context->allocator(), captureBuffer.allocation, &info);
    vmaInvalidateAllocation(context->allocator(), captureBuffer.allocation, 0, VK_WHOLE_SIZE);
    // The scene target is RGBA16F (kSceneColorFormat): half floats, rows top to bottom.
    const auto* halves = static_cast<const uint16_t*>(info.pMappedData);
    capture.width = captureExtent.width;
    capture.height = captureExtent.height;
    capture.rgba.resize(size_t(capture.width) * capture.height * 4);
    for (size_t i = 0; i < capture.rgba.size(); ++i) {
        const float v = glm::unpackHalf1x16(halves[i]);
        capture.rgba[i] = std::isfinite(v) ? v : 0.0f;
    }
    vmaDestroyBuffer(context->allocator(), captureBuffer.buffer, captureBuffer.allocation);
    captureBuffer = {};
}

}   // namespace shockwave
