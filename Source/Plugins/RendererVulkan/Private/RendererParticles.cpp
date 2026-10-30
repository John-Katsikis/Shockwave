// Particles (Renderer/Particles.h): world-space geometry built on the CPU by the Effects
// module, drawn in its own render graph pass after the scene, into the HDR image and
// depth tested against it. One pipeline per blend mode; cull mode and depth test and
// write are dynamic state, so a batch never needs a pipeline of its own.
#include "RendererImpl.h"

#include <Core/Profile.h>

#include <chrono>

namespace shockwave {

namespace {

using Clock = std::chrono::steady_clock;
float msSince(Clock::time_point start) {
    return std::chrono::duration<float, std::milli>(Clock::now() - start).count();
}

// Must match the push constants in shaders/particle.vert and particle.frag.
struct ParticlePushConstants {
    glm::mat4 viewProjection;
    VkDeviceAddress vertices;
    VkDeviceAddress batches;
};
static_assert(sizeof(ParticlePushConstants) == 80, "ParticlePushConstants must match the particle shaders");

void setBlend(VkPipelineColorBlendAttachmentState& state, ParticleBlend blend) {
    state.blendEnable = blend != ParticleBlend::Opaque;
    state.colorBlendOp = VK_BLEND_OP_ADD;
    state.alphaBlendOp = VK_BLEND_OP_ADD;
    state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;   // keep the scene's alpha
    state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    switch (blend) {
    case ParticleBlend::Opaque: break;
    case ParticleBlend::Alpha:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case ParticleBlend::Add:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case ParticleBlend::Subtract:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        state.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
        break;
    case ParticleBlend::Multiply:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        break;
    }
}

VkCullModeFlags cullMode(ParticleCull cull) {
    switch (cull) {
    case ParticleCull::Front: return VK_CULL_MODE_FRONT_BIT;
    case ParticleCull::Back: return VK_CULL_MODE_BACK_BIT;
    case ParticleCull::None: break;
    }
    return VK_CULL_MODE_NONE;
}

}   // namespace

void VulkanRenderer::Impl::createParticlePipelines() {
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = sizeof(ParticlePushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &bindlessLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    VkDevice device = context->device();
    vk::check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &particleLayout), "vkCreatePipelineLayout");

    VkShaderModule vert = vk::loadShaderModule(device, shaderPath("particle.vert"));
    VkShaderModule frag = vk::loadShaderModule(device, shaderPath("particle.frag"));
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};   // vertices are pulled by address
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;   // reverse-Z
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    // Core in Vulkan 1.3: one pipeline per blend mode covers every batch.
    const VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<uint32_t>(std::size(dynamicStates));
    dynamic.pDynamicStates = dynamicStates;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &kSceneColorFormat;
    rendering.depthAttachmentFormat = kDepthFormat;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = particleLayout;
    VkResult result = VK_SUCCESS;
    for (size_t i = 0; i < particlePipelines.size() && result == VK_SUCCESS; ++i) {
        setBlend(blendAttachment, static_cast<ParticleBlend>(i));
        result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &particlePipelines[i]);
    }
    vkDestroyShaderModule(device, frag, nullptr);
    vkDestroyShaderModule(device, vert, nullptr);
    vk::check(result, "vkCreateGraphicsPipelines");
}

void VulkanRenderer::Impl::destroyParticles() {
    VkDevice device = context->device();
    for (VkPipeline& pipeline : particlePipelines) {
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if (particleLayout) vkDestroyPipelineLayout(device, particleLayout, nullptr);
    particleLayout = VK_NULL_HANDLE;
    for (ParticleBuffers& buffers : particleBuffers) {
        for (vk::Buffer* buffer : {&buffers.vertices, &buffers.indices, &buffers.batches})
            if (buffer->buffer) vmaDestroyBuffer(context->allocator(), buffer->buffer, buffer->allocation);
        buffers = {};
    }
}

void VulkanRenderer::Impl::drawParticles(const ParticleDrawList& list) {
    SW_PROFILE_SCOPE("Particles gather");
    const auto start = Clock::now();
    // Appended to CPU arrays that keep their capacity, so a steady frame allocates
    // nothing; prepareParticles copies them to the GPU once, at the end of the frame.
    const uint32_t vertexBase = static_cast<uint32_t>(particleVertices.size());
    const uint32_t indexBase = static_cast<uint32_t>(particleIndices.size());
    particleVertices.insert(particleVertices.end(), list.vertices.begin(), list.vertices.end());
    particleIndices.reserve(particleIndices.size() + list.indices.size());
    for (uint32_t index : list.indices) particleIndices.push_back(index + vertexBase);
    const glm::vec3 lightDirection =
        glm::length(list.lightDirection) > 0.0f ? glm::normalize(list.lightDirection) : glm::vec3(0, -1, 0);
    for (const ParticleBatch& batch : list.batches) {
        if (batch.indexCount == 0 || batch.firstIndex + batch.indexCount > list.indices.size()) continue;
        ParticleBatchData data{};
        data.tint = batch.tint;
        data.edgeColor = batch.edgeColor;
        data.lightDirection = lightDirection;
        data.texture = isTexture(batch.texture.id) ? textureSlot(batch.texture.id) : 0;
        data.lightColor = list.lightColor;
        data.alphaTexture = isTexture(batch.alphaTexture.id) ? textureSlot(batch.alphaTexture.id) : 0;
        data.ambient = list.ambient;
        data.flags = batch.flags;
        data.emissive = batch.emissive;
        data.blend = static_cast<uint32_t>(batch.blend);
        particleBatchData.push_back(data);
        particleDraws.push_back(
            {indexBase + batch.firstIndex, batch.indexCount, batch.blend, batch.cull, batch.depthTest,
             batch.depthWrite});
    }
    particleGatherMs += msSince(start);
}

void VulkanRenderer::Impl::clearParticles() {
    particleVertices.clear();
    particleIndices.clear();
    particleBatchData.clear();
    particleDraws.clear();
    particleGatherMs = 0;
}

void VulkanRenderer::Impl::prepareParticles() {
    lastParticleStats = {};
    if (particleDraws.empty()) return;
    SW_PROFILE_SCOPE("Particles upload");
    const auto start = Clock::now();
    // beginFrame waited for the GPU to finish this slot's previous frame, so its buffers
    // are free to overwrite, or to replace when they're too small.
    ParticleBuffers& b = particleBuffers[frameIndex];
    const VmaAllocationCreateFlags mapped =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    // True when the buffer was replaced. (Its handle can't tell: a new buffer may reuse the
    // old one's handle value.)
    auto fit = [&](vk::Buffer& buffer, void*& data, size_t& capacity, size_t needed, size_t minimum,
                   VkBufferUsageFlags usage) {
        if (needed <= capacity) return false;
        size_t grown = std::max(capacity, minimum);
        while (grown < needed) grown *= 2;
        if (buffer.buffer) vmaDestroyBuffer(context->allocator(), buffer.buffer, buffer.allocation);
        VmaAllocationInfo info{};
        buffer = createBuffer(grown, usage, mapped, &info);
        data = info.pMappedData;
        capacity = grown;
        return true;
    };
    const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const size_t vertexBytes = particleVertices.size() * sizeof(ParticleVertex);
    const size_t indexBytes = particleIndices.size() * sizeof(uint32_t);
    const size_t batchBytes = particleBatchData.size() * sizeof(ParticleBatchData);
    if (fit(b.vertices, b.vertexData, b.vertexCapacity, vertexBytes, size_t{1} << 20, storage))
        b.vertexAddress = bufferAddress(b.vertices.buffer);
    fit(b.indices, b.indexData, b.indexCapacity, indexBytes, size_t{1} << 19, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (fit(b.batches, b.batchData, b.batchCapacity, batchBytes, size_t{1} << 14, storage))
        b.batchAddress = bufferAddress(b.batches.buffer);
    std::memcpy(b.vertexData, particleVertices.data(), vertexBytes);
    std::memcpy(b.indexData, particleIndices.data(), indexBytes);
    std::memcpy(b.batchData, particleBatchData.data(), batchBytes);
    VmaAllocator allocator = context->allocator();
    vk::check(vmaFlushAllocation(allocator, b.vertices.allocation, 0, vertexBytes), "vmaFlushAllocation");
    vk::check(vmaFlushAllocation(allocator, b.indices.allocation, 0, indexBytes), "vmaFlushAllocation");
    vk::check(vmaFlushAllocation(allocator, b.batches.allocation, 0, batchBytes), "vmaFlushAllocation");
    lastParticleStats.uploadMs = particleGatherMs + msSince(start);
    lastParticleStats.uploadBytes = vertexBytes + indexBytes + batchBytes;
    lastParticleStats.vertices = static_cast<uint32_t>(particleVertices.size());
    lastParticleStats.draws = static_cast<uint32_t>(particleDraws.size());
}

void VulkanRenderer::Impl::recordParticles(VkCommandBuffer cmd) {
    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = sceneColor.view;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // on top of the scene
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = sceneDepth.view;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, sceneColor.extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &colorAttachment;
    rendering.pDepthAttachment = &depthAttachment;
    vkCmdBeginRendering(cmd, &rendering);
    setSceneViewport(cmd);

    const ParticleBuffers& b = particleBuffers[frameIndex];
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particleLayout, 0, 1, &bindlessSet, 0, nullptr);
    vkCmdBindIndexBuffer(cmd, b.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    const ParticlePushConstants push{viewProjection, b.vertexAddress, b.batchAddress};
    VkPipeline bound = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < particleDraws.size(); ++i) {
        const ParticleDraw& draw = particleDraws[i];
        const VkPipeline pipeline = particlePipelines[static_cast<size_t>(draw.blend)];
        if (pipeline != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            if (!bound) {
                vkCmdPushConstants(
                    cmd, particleLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                    &push);
            }
            bound = pipeline;
        }
        vkCmdSetCullMode(cmd, cullMode(draw.cull));
        vkCmdSetDepthTestEnable(cmd, draw.depthTest);
        vkCmdSetDepthWriteEnable(cmd, draw.depthWrite);
        vkCmdDrawIndexed(cmd, draw.indexCount, 1, draw.firstIndex, 0, i);   // instance i: its batch
    }
    vkCmdEndRendering(cmd);
}

}   // namespace shockwave
