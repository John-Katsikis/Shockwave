// The game UI overlay (GameUI module): 2D geometry drawn into the display image after
// tonemapping, at output resolution, so text stays sharp at any window size and
// screenshots and the editor viewport include it.
#include "RendererImpl.h"

#include <Core/Profile.h>

namespace shockwave {

void VulkanRenderer::Impl::createUiPipeline() {
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = sizeof(UiPushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &bindlessLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    vk::check(vkCreatePipelineLayout(context->device(), &layoutInfo, nullptr, &uiLayout), "vkCreatePipelineLayout");

    vk::GraphicsPipelineDesc desc;
    desc.vertexShader = shaderPath("ui.vert");
    desc.fragmentShader = shaderPath("ui.frag");
    desc.colorFormat = kUiTargetFormat;
    desc.layout = uiLayout;
    desc.cullMode = VK_CULL_MODE_NONE;   // CSS transforms can flip geometry
    desc.premultipliedAlpha = true;
    uiPipeline = vk::createGraphicsPipeline(context->device(), desc);
}

void VulkanRenderer::Impl::destroyUiPipeline() {
    vkDestroyPipeline(context->device(), uiPipeline, nullptr);
    vkDestroyPipelineLayout(context->device(), uiLayout, nullptr);
    uiPipeline = VK_NULL_HANDLE;
    uiLayout = VK_NULL_HANDLE;
}

void VulkanRenderer::Impl::createDisplayUiView() {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = displayColor.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kUiTargetFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk::check(vkCreateImageView(context->device(), &viewInfo, nullptr, &displayUiView), "vkCreateImageView");
}

void VulkanRenderer::Impl::destroyDisplayUiView() {
    if (displayUiView) vkDestroyImageView(context->device(), displayUiView, nullptr);
    displayUiView = VK_NULL_HANDLE;
}

void VulkanRenderer::Impl::destroyUiBuffers(Frame& f) {
    VmaAllocator allocator = context->allocator();
    if (f.uiVertexBuffer.buffer) vmaDestroyBuffer(allocator, f.uiVertexBuffer.buffer, f.uiVertexBuffer.allocation);
    if (f.uiIndexBuffer.buffer) vmaDestroyBuffer(allocator, f.uiIndexBuffer.buffer, f.uiIndexBuffer.allocation);
    f.uiVertexBuffer = {};
    f.uiIndexBuffer = {};
    f.uiVertices = nullptr;
    f.uiIndices = nullptr;
    f.uiVertexCapacity = f.uiIndexCapacity = 0;
}

void VulkanRenderer::Impl::drawUi(const UiDrawList& list) {
    SW_PROFILE_SCOPE("UI upload");
    if (list.commands.empty() || list.vertices.empty() || list.indices.empty() || !list.size.x || !list.size.y) return;
    // beginFrame waited for the GPU to finish this slot's previous frame, so its buffers
    // are free to overwrite, or to replace when they're too small.
    Frame& f = frames[frameIndex];
    const VmaAllocationCreateFlags mapped =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    if (list.vertices.size() > f.uiVertexCapacity) {
        size_t capacity = std::max<size_t>(f.uiVertexCapacity, 16384);
        while (capacity < list.vertices.size()) capacity *= 2;
        if (f.uiVertexBuffer.buffer)
            vmaDestroyBuffer(context->allocator(), f.uiVertexBuffer.buffer, f.uiVertexBuffer.allocation);
        VmaAllocationInfo info{};
        f.uiVertexBuffer = createBuffer(
            capacity * sizeof(UiVertex), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            mapped, &info);
        f.uiVertices = static_cast<UiVertex*>(info.pMappedData);
        f.uiVertexAddress = bufferAddress(f.uiVertexBuffer.buffer);
        f.uiVertexCapacity = capacity;
    }
    if (list.indices.size() > f.uiIndexCapacity) {
        size_t capacity = std::max<size_t>(f.uiIndexCapacity, 49152);
        while (capacity < list.indices.size()) capacity *= 2;
        if (f.uiIndexBuffer.buffer)
            vmaDestroyBuffer(context->allocator(), f.uiIndexBuffer.buffer, f.uiIndexBuffer.allocation);
        VmaAllocationInfo info{};
        f.uiIndexBuffer = createBuffer(capacity * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, mapped, &info);
        f.uiIndices = static_cast<uint32_t*>(info.pMappedData);
        f.uiIndexCapacity = capacity;
    }
    std::memcpy(f.uiVertices, list.vertices.data(), list.vertices.size_bytes());
    std::memcpy(f.uiIndices, list.indices.data(), list.indices.size_bytes());
    vk::check(
        vmaFlushAllocation(context->allocator(), f.uiVertexBuffer.allocation, 0, list.vertices.size_bytes()),
        "vmaFlushAllocation");
    vk::check(
        vmaFlushAllocation(context->allocator(), f.uiIndexBuffer.allocation, 0, list.indices.size_bytes()),
        "vmaFlushAllocation");
    uiCommands.assign(list.commands.begin(), list.commands.end());
    uiTransforms.assign(list.transforms.begin(), list.transforms.end());
    uiLayoutSize = list.size;
}

void VulkanRenderer::Impl::recordUi(VkCommandBuffer cmd, Frame& f) {
    VkRenderingAttachmentInfo target{};
    target.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    target.imageView = displayUiView;
    target.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    target.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // on top of the tonemapped scene
    target.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    const VkExtent2D extent = displayColor.extent;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &target;
    vkCmdBeginRendering(cmd, &rendering);

    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                              0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uiPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uiLayout, 0, 1, &bindlessSet, 0, nullptr);
    vkCmdBindIndexBuffer(cmd, f.uiIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    // Layout pixels to clip space. The UI was laid out for uiLayoutSize; if the target was
    // resized since (one frame at most), it stretches to fit rather than spilling over.
    const float width = static_cast<float>(uiLayoutSize.x), height = static_cast<float>(uiLayoutSize.y);
    glm::mat4 ortho(1.0f);
    ortho[0][0] = 2.0f / width;
    ortho[1][1] = 2.0f / height;
    ortho[3][0] = -1.0f;
    ortho[3][1] = -1.0f;
    const float scaleX = static_cast<float>(extent.width) / width;
    const float scaleY = static_cast<float>(extent.height) / height;

    UiPushConstants push{};
    push.vertices = f.uiVertexAddress;
    uint32_t boundTransform = kNoUiTransform - 1;   // nothing pushed yet
    VkRect2D boundScissor{{-1, -1}, {0, 0}};
    for (const UiDrawCommand& command : uiCommands) {
        if (command.indexCount == 0) continue;
        VkRect2D scissor{{0, 0}, extent};
        if (command.scissor.z >= 0) {
            const int32_t x0 = std::clamp(
                static_cast<int32_t>(std::floor(static_cast<float>(command.scissor.x) * scaleX)), 0,
                static_cast<int32_t>(extent.width));
            const int32_t y0 = std::clamp(
                static_cast<int32_t>(std::floor(static_cast<float>(command.scissor.y) * scaleY)), 0,
                static_cast<int32_t>(extent.height));
            const int32_t x1 = std::clamp(
                static_cast<int32_t>(std::ceil(static_cast<float>(command.scissor.x + command.scissor.z) * scaleX)), x0,
                static_cast<int32_t>(extent.width));
            const int32_t y1 = std::clamp(
                static_cast<int32_t>(std::ceil(static_cast<float>(command.scissor.y + command.scissor.w) * scaleY)), y0,
                static_cast<int32_t>(extent.height));
            if (x1 == x0 || y1 == y0) continue;   // clipped away entirely
            scissor = {{x0, y0}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
        }
        if (std::memcmp(&scissor, &boundScissor, sizeof(scissor)) != 0) {
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            boundScissor = scissor;
        }
        const uint32_t texture = isTexture(command.texture.id) ? textureSlot(command.texture.id) : 0;
        if (command.transform != boundTransform || texture != push.texture || boundTransform == kNoUiTransform - 1) {
            push.transform = command.transform < uiTransforms.size() ? ortho * uiTransforms[command.transform] : ortho;
            push.texture = texture;
            boundTransform = command.transform;
            vkCmdPushConstants(
                cmd, uiLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        }
        vkCmdDrawIndexed(cmd, command.indexCount, 1, command.firstIndex, 0, 0);
    }
    vkCmdEndRendering(cmd);
}

}   // namespace shockwave
