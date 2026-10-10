#include "RendererImpl.h"

namespace shockwave {

vk::Buffer VulkanRenderer::Impl::createBuffer(
    VkDeviceSize size, VkBufferUsageFlags usage, VmaAllocationCreateFlags flags, VmaAllocationInfo* outInfo) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = flags;

    vk::Buffer buffer;
    vk::check(
        vmaCreateBuffer(context->allocator(), &bufferInfo, &allocInfo, &buffer.buffer, &buffer.allocation, outInfo),
        "vmaCreateBuffer");
    return buffer;
}

VkDeviceAddress VulkanRenderer::Impl::bufferAddress(VkBuffer buffer) {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(context->device(), &info);
}

vk::Buffer VulkanRenderer::Impl::createStaging(const void* data, VkDeviceSize size) {
    VmaAllocationInfo info{};
    vk::Buffer staging = createBuffer(
        size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, &info);
    std::memcpy(info.pMappedData, data, size);
    vk::check(vmaFlushAllocation(context->allocator(), staging.allocation, 0, VK_WHOLE_SIZE), "vmaFlushAllocation");
    return staging;
}

void VulkanRenderer::Impl::immediateSubmit(const std::function<void(VkCommandBuffer)>& record) {
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

vk::Buffer VulkanRenderer::Impl::uploadBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
    vk::Buffer staging = createStaging(data, size);
    vk::Buffer buffer = createBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0);
    immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy region{0, 0, size};
        vkCmdCopyBuffer(cmd, staging.buffer, buffer.buffer, 1, &region);
    });
    vmaDestroyBuffer(context->allocator(), staging.buffer, staging.allocation);
    return buffer;
}

void VulkanRenderer::Impl::createMesh(
    MeshHandle handle, std::span<const Vertex> vertices, std::span<const uint32_t> indices) {
    const uint32_t id = handle.id;
    if (id >= kMaxMeshes || meshReady[id].load()) throw std::runtime_error("createMesh: mesh id is taken");
    Mesh& mesh = meshes[id];
    try {
        mesh.vertices = uploadBuffer(
            vertices.data(), vertices.size_bytes(),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        mesh.indices = uploadBuffer(indices.data(), indices.size_bytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        mesh.indexCount = static_cast<uint32_t>(indices.size());
        // A bounding sphere for culling: centered on the bounding box, wide enough for every vertex.
        glm::vec3 lo(vertices[0].position), hi(vertices[0].position);
        for (const Vertex& v : vertices) {
            lo = glm::min(lo, v.position);
            hi = glm::max(hi, v.position);
        }
        const glm::vec3 center = (lo + hi) * 0.5f;
        float radius2 = 0.0f;
        for (const Vertex& v : vertices)
            radius2 = std::max(radius2, glm::dot(v.position - center, v.position - center));
        mesh.bounds = glm::vec4(center, std::sqrt(radius2));
        mesh.vertexAddress = bufferAddress(mesh.vertices.buffer);
        meshReady[id].store(true, std::memory_order_release);
    } catch (...) {
        if (mesh.vertices.buffer)
            vmaDestroyBuffer(context->allocator(), mesh.vertices.buffer, mesh.vertices.allocation);
        if (mesh.indices.buffer) vmaDestroyBuffer(context->allocator(), mesh.indices.buffer, mesh.indices.allocation);
        mesh = {};
        throw;
    }
}

void VulkanRenderer::Impl::destroyMesh(MeshHandle handle) {
    if (handle.id >= kMaxMeshes || !meshReady[handle.id].load()) return;
    vk::check(vkDeviceWaitIdle(context->device()), "vkDeviceWaitIdle");
    Mesh& mesh = meshes[handle.id];
    meshReady[handle.id].store(false, std::memory_order_release);
    vmaDestroyBuffer(context->allocator(), mesh.vertices.buffer, mesh.vertices.allocation);
    vmaDestroyBuffer(context->allocator(), mesh.indices.buffer, mesh.indices.allocation);
    mesh = {};
}

void VulkanRenderer::Impl::destroyTexture(TextureHandle handle) {
    if (!handle.id || !isTexture(handle.id)) return;
    vk::check(vkDeviceWaitIdle(context->device()), "vkDeviceWaitIdle");
    if (auto it = guiTextures.find(handle.id); it != guiTextures.end()) {
        ImGui_ImplVulkan_RemoveTexture(it->second);
        guiTextures.erase(it);
    }
    {
        std::lock_guard lock(textureSwapMutex);
        for (auto& swap : textureSwaps)
            if (swap.id == handle.id) destroyImage(swap.image);
        std::erase_if(textureSwaps, [&](const auto& swap) { return swap.id == handle.id; });
    }
    destroyImage(textures[handle.id]);
    // Fill the descriptor with white before recycling; don't own the white image.
    textures[handle.id] = textures[0];
    writeTextureDescriptor(handle.id);
    textures[handle.id] = {};
}

void VulkanRenderer::Impl::createTexture(
    TextureHandle handle, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) {
    if (!width || !height || rgba.size() < size_t(width) * height * 4)
        throw std::runtime_error("createTexture: pixel data is smaller than width x height x 4");
    const uint32_t id = handle.id;
    if (id >= textureCapacity() || textures[id].image) throw std::runtime_error("createTexture: texture id is taken");
    textures[id] = uploadTexture(width, height, rgba, srgb);
    writeTextureDescriptor(id);
}

void VulkanRenderer::Impl::updateTexture(
    TextureHandle texture, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) {
    if (width == 0 || height == 0 || rgba.size() < size_t(width) * height * 4) {
        throw std::runtime_error("updateTexture: pixel data is smaller than width x height x 4");
    }
    if (texture.id == 0 || texture.id >= textureCapacity()) {
        throw std::runtime_error("updateTexture: not a texture made with createTexture");
    }
    vk::Image image = uploadTexture(width, height, rgba, srgb);
    std::lock_guard lock(textureSwapMutex);
    textureSwaps.push_back({texture.id, image});
}

void VulkanRenderer::Impl::applyTextureSwaps() {
    std::vector<TextureSwap> swaps;
    {
        std::lock_guard lock(textureSwapMutex);
        swaps.swap(textureSwaps);
    }
    if (swaps.empty()) return;
    if (frameNumber > 0) {
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &frameTimeline;
        waitInfo.pValues = &frameNumber;
        vk::check(vkWaitSemaphores(context->device(), &waitInfo, UINT64_MAX), "vkWaitSemaphores");
    }
    for (TextureSwap& swap : swaps) {
        destroyImage(textures[swap.id]);
        textures[swap.id] = swap.image;
        writeTextureDescriptor(swap.id);
        // The GUI's copy points at the old view: make a new one next time it's asked for.
        if (auto it = guiTextures.find(swap.id); it != guiTextures.end()) {
            ImGui_ImplVulkan_RemoveTexture(it->second);
            guiTextures.erase(it);
        }
    }
}

uint64_t VulkanRenderer::Impl::guiTexture(TextureHandle texture) {
    if (!guiEnabled || texture.id >= textureCapacity()) return 0;
    if (auto it = guiTextures.find(texture.id); it != guiTextures.end()) {
        return reinterpret_cast<uint64_t>(it->second);
    }
    const VkImageView view = textures[texture.id].view;
    if (!view) return 0;   // still being created on another thread
    const VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    guiTextures[texture.id] = set;
    return reinterpret_cast<uint64_t>(set);
}

void VulkanRenderer::Impl::writeTextureDescriptor(uint32_t id) {
    std::lock_guard lock(descriptorMutex);
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = textures[id].view;
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

vk::Image
VulkanRenderer::Impl::uploadTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) {
    const uint32_t mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
    const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    vk::Image image = createImage(
        {width, height}, format,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);

    vk::Buffer staging = createStaging(rgba.data(), size_t(width) * height * 4);
    immediateSubmit([&](VkCommandBuffer cmd) {
        transitionImage(
            cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
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
            transitionImage(
                cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, level - 1,
                1);
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
            transitionImage(
                cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, 0,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, mipLevels - 1);
        }
        transitionImage(
            cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, mipLevels - 1, 1);
    });
    vmaDestroyBuffer(context->allocator(), staging.buffer, staging.allocation);
    return image;
}

}   // namespace shockwave
