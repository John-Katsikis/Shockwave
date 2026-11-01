// Particles (Renderer/Particles.h): world-space geometry built on the CPU by the Effects
// module, drawn in its own render graph pass after the scene, into the HDR image and
// depth tested against it. One pipeline per blend mode; cull mode and depth test and
// write are dynamic state, so a batch never needs a pipeline of its own.
//
// A04: with soft particles the pass keeps depth read-only and samples it, so batches that
// write depth (opaque models) are drawn first, in a pass of their own. Distortion draws
// read a copy of the scene made just before the particle pass.
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
    VkDeviceAddress frame;   // FrameBuffer (frame.glsl): camera, lights, clusters
    uint32_t depthSlot;   // scene depth for soft particles and distortion; 0: don't read it
    uint32_t backgroundSlot;   // the scene before particles, for distortion; 0: none
    uint32_t flags;   // kParticlePass*
    uint32_t pad;
};
// ParticlePushConstants::flags, as in particle.frag.
constexpr uint32_t kParticlePassSceneLighting = 1u << 0;   // lit batches take every light (particle_lighting scene)
constexpr uint32_t kParticlePassSoft = 1u << 1;   // soft batches fade against scene depth (soft_particles)
static_assert(sizeof(ParticlePushConstants) == 104, "ParticlePushConstants must match the particle shaders");

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
        result = vkCreateGraphicsPipelines(device, vk::pipelineCache(), 1, &info, nullptr, &particlePipelines[i]);
    }
    // The reactive mask's (G09): one channel, the greatest of what each particle writes.
    const VkBool32 reactive = VK_TRUE;
    const VkSpecializationMapEntry entry{0, 0, sizeof(VkBool32)};
    const VkSpecializationInfo specialization{1, &entry, sizeof(reactive), &reactive};
    stages[1].pSpecializationInfo = &specialization;
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.srcAlphaBlendFactor = blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.colorBlendOp = blendAttachment.alphaBlendOp = VK_BLEND_OP_MAX;
    const VkFormat reactiveFormat = VK_FORMAT_R8_UNORM;
    rendering.pColorAttachmentFormats = &reactiveFormat;
    if (result == VK_SUCCESS)
        result = vkCreateGraphicsPipelines(device, vk::pipelineCache(), 1, &info, nullptr, &reactiveParticlePipeline);
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
    if (reactiveParticlePipeline) vkDestroyPipeline(device, reactiveParticlePipeline, nullptr);
    reactiveParticlePipeline = VK_NULL_HANDLE;
    if (particleLayout) vkDestroyPipelineLayout(device, particleLayout, nullptr);
    particleLayout = VK_NULL_HANDLE;
    destroyParticleBackground(particleBackground);
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
    const bool distortion = particleDistortion.load(std::memory_order_relaxed);
    for (const ParticleBatch& batch : list.batches) {
        if (batch.indexCount == 0 || batch.firstIndex + batch.indexCount > list.indices.size()) continue;
        if ((batch.flags & kParticleDistortion) && !distortion) {
            ++particleSkippedDistortion;   // particle_distortion off
            continue;
        }
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
        data.softDistance = std::max(batch.softDistance, 0.0f);
        data.softNear = std::max(batch.softNear, 0.0f);
        data.softNearOffset = batch.softNearOffset;
        data.distortion = batch.distortion;
        particleBatchData.push_back(data);
        // Opaque batches don't blend, so they have nothing to fade.
        const bool soft = batch.blend != ParticleBlend::Opaque && (data.softDistance > 0.0f || data.softNear > 0.0f);
        particleDraws.push_back(
            {indexBase + batch.firstIndex, batch.indexCount, batch.blend, batch.cull, batch.depthTest, batch.depthWrite,
             soft, (batch.flags & kParticleDistortion) != 0});
    }
    particleGatherMs += msSince(start);
}

void VulkanRenderer::Impl::clearParticles() {
    particleVertices.clear();
    particleIndices.clear();
    particleBatchData.clear();
    particleDraws.clear();
    particleGatherMs = 0;
    particleSkippedDistortion = 0;
}

void VulkanRenderer::Impl::prepareParticles() {
    lastParticleStats = {};
    particleSoftThisFrame = particleDepthPassThisFrame = particleBackgroundThisFrame = false;
    // Distortion off: drawParticles left its batches out, and its copy of the scene is freed.
    const bool distortion = particleDistortion.load(std::memory_order_relaxed);
    lastParticleStats.skippedDistortion = particleSkippedDistortion;
    bool anySoft = false, anyDistortion = false, anyDepthWrite = false;
    for (const ParticleDraw& d : particleDraws) {
        anySoft |= d.soft;
        anyDistortion |= d.distortion;
        anyDepthWrite |= d.depthWrite;
    }
    updateParticleBackground(distortion && (anyDistortion || particleBackground.image.image));
    if (particleDraws.empty()) return;
    // Soft particles and distortion read scene depth, so the main pass can't write it.
    particleSoftThisFrame =
        (anySoft && softParticles.load(std::memory_order_relaxed)) || (anyDistortion && particleBackground.image.image);
    particleBackgroundThisFrame = anyDistortion && particleBackground.image.image;
    particleDepthPassThisFrame = particleSoftThisFrame && anyDepthWrite;
    for (const ParticleDraw& d : particleDraws) {
        lastParticleStats.softDraws += particleSoftThisFrame && d.soft && softParticles.load(std::memory_order_relaxed);
        lastParticleStats.distortionDraws += d.distortion && particleBackgroundThisFrame;
        lastParticleStats.sceneLitDraws +=
            particleLighting.load(std::memory_order_relaxed) == ParticleLighting::Scene &&
            (particleBatchData[&d - particleDraws.data()].flags & kParticleLit) != 0;
    }
    lastParticleStats.depthPass = particleDepthPassThisFrame;
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

void VulkanRenderer::Impl::destroyParticleBackground(ParticleBackground& background) {
    destroyImage(background.image);
    background = {};
}

void VulkanRenderer::Impl::updateParticleBackground(bool want) {
    const VkExtent2D extent = sceneColor.extent;
    const bool fits = particleBackground.image.image && particleBackground.image.extent.width == extent.width &&
                      particleBackground.image.extent.height == extent.height;
    if (want == static_cast<bool>(particleBackground.image.image) && (!want || fits)) return;
    auto retireCurrent = [this] {
        if (!particleBackground.image.image) return;
        const int slot = particleBackground.slot == particleBackgroundSlots[0] ? 0 : 1;
        particleBackgroundSlotBusy[slot] = true;
        retire([this, old = particleBackground, slot]() mutable {
            destroyParticleBackground(old);
            particleBackgroundSlotBusy[slot] = false;
        });
        particleBackground = {};
    };
    if (!want) {
        retireCurrent();
        return;
    }
    // A slot no frame in flight reads; none free (both just replaced): the next frame tries again.
    int free = -1;
    for (int i = 0; i < 2 && free < 0; ++i)
        if (!particleBackgroundSlotBusy[i] &&
            !(particleBackground.image.image && particleBackgroundSlots[i] == particleBackground.slot))
            free = i;
    if (free < 0) return;
    ParticleBackground next;
    next.slot = particleBackgroundSlots[free];
    next.image = createImage(
        extent, kSceneColorFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, 1, true);
    {
        std::lock_guard lock(descriptorMutex);
        VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, next.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = bindlessSet;
        write.dstBinding = 0;
        write.dstArrayElement = next.slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(context->device(), 1, &write, 0, nullptr);
    }
    retireCurrent();
    particleBackground = next;
    particleBackgroundState = {};   // a new image starts undefined
}

void VulkanRenderer::Impl::recordParticleBackgroundCopy(VkCommandBuffer cmd) {
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {sceneColor.extent.width, sceneColor.extent.height, 1};
    vkCmdCopyImage(
        cmd, sceneColor.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, particleBackground.image.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void VulkanRenderer::Impl::recordParticles(VkCommandBuffer cmd, bool depthPass) {
    // The depth-writing pass (soft particles on) and the plain pass write depth; the main
    // pass with soft particles or distortion keeps it read-only, and samples it.
    const bool readOnly = particleSoftThisFrame && !depthPass;
    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = sceneColor.view;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // on top of the scene
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = sceneDepth.view;
    depthAttachment.imageLayout =
        readOnly ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    // Kept for the main pass after the depth-writing one, and for the reactive mask and TAA (G09, G10).
    depthAttachment.storeOp =
        temporalThisFrame || depthPass || readOnly ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
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
    ParticlePushConstants push{viewProjection, b.vertexAddress, b.batchAddress, particleFrameAddress, 0, 0, 0, 0};
    if (readOnly) push.depthSlot = sceneDepthSlot;
    if (readOnly && softParticles.load(std::memory_order_relaxed)) push.flags |= kParticlePassSoft;
    if (particleBackgroundThisFrame) push.backgroundSlot = particleBackground.slot;
    if (particleLighting.load(std::memory_order_relaxed) == ParticleLighting::Scene)
        push.flags |= kParticlePassSceneLighting;
    VkPipeline bound = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < particleDraws.size(); ++i) {
        const ParticleDraw& draw = particleDraws[i];
        // With a depth-writing pass, it draws the opaque batches that write depth and the
        // main pass the rest; without one, every batch is drawn here.
        const bool early = particleDepthPassThisFrame && draw.depthWrite && draw.blend == ParticleBlend::Opaque;
        if (particleDepthPassThisFrame && early != depthPass) continue;
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
        // See-through batches that ask to write depth can't over read-only depth.
        vkCmdSetDepthWriteEnable(cmd, draw.depthWrite && !readOnly);
        vkCmdDrawIndexed(cmd, draw.indexCount, 1, draw.firstIndex, 0, i);   // instance i: its batch
    }
    vkCmdEndRendering(cmd);
}

void VulkanRenderer::Impl::recordParticleReactive(VkCommandBuffer cmd) {
    // Inside the reactive pass: the same batches, depth tested as drawn but never written.
    // No depth or scene reads: each batch's coverage, unfaded, is what TAA needs to know.
    const ParticleBuffers& b = particleBuffers[frameIndex];
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, reactiveParticlePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particleLayout, 0, 1, &bindlessSet, 0, nullptr);
    vkCmdBindIndexBuffer(cmd, b.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    ParticlePushConstants push{viewProjection, b.vertexAddress, b.batchAddress, particleFrameAddress, 0, 0, 0, 0};
    if (particleLighting.load(std::memory_order_relaxed) == ParticleLighting::Scene)
        push.flags |= kParticlePassSceneLighting;
    vkCmdPushConstants(
        cmd, particleLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
    for (uint32_t i = 0; i < particleDraws.size(); ++i) {
        const ParticleDraw& draw = particleDraws[i];
        vkCmdSetCullMode(cmd, cullMode(draw.cull));
        vkCmdSetDepthTestEnable(cmd, draw.depthTest);
        vkCmdDrawIndexed(cmd, draw.indexCount, 1, draw.firstIndex, 0, i);
    }
}

}   // namespace shockwave
