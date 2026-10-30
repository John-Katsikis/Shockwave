#include "RendererImpl.h"

namespace shockwave {

void VulkanRenderer::Impl::createFrames() {
    VkDevice device = context->device();
    for (Frame& f : frames) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = context->graphicsQueueFamily();
        vk::check(vkCreateCommandPool(device, &poolInfo, nullptr, &f.pool), "vkCreateCommandPool");

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = f.pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        vk::check(vkAllocateCommandBuffers(device, &allocInfo, &f.cmd), "vkAllocateCommandBuffers");

        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vk::check(vkCreateSemaphore(device, &semInfo, nullptr, &f.imageAcquired), "vkCreateSemaphore");

        ensureDrawCapacity(f, 4096);
        VmaAllocationInfo info{};
        f.frameBuffer = createBuffer(
            kFrameDataSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, &info);
        f.frameData = static_cast<uint8_t*>(info.pMappedData);
        f.frameAddress = bufferAddress(f.frameBuffer.buffer);

        if (context->timestampPeriod() > 0.0f) {
            VkQueryPoolCreateInfo queryInfo{};
            queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = vk::RenderGraph::kMaxTimedPasses + 1;
            vk::check(vkCreateQueryPool(device, &queryInfo, nullptr, &f.timestamps), "vkCreateQueryPool");
        }
    }

    VkSemaphoreTypeCreateInfo timelineInfo{};
    timelineInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineInfo.initialValue = 0;
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semInfo.pNext = &timelineInfo;
    vk::check(vkCreateSemaphore(device, &semInfo, nullptr, &frameTimeline), "vkCreateSemaphore");
}

void VulkanRenderer::Impl::ensureDrawCapacity(Frame& f, uint32_t count) {
    if (count <= f.drawCapacity) return;
    uint32_t capacity = std::max<uint32_t>(f.drawCapacity, 4096);
    while (capacity < count) capacity *= 2;

    if (f.drawBuffer.buffer) vmaDestroyBuffer(context->allocator(), f.drawBuffer.buffer, f.drawBuffer.allocation);
    VmaAllocationInfo info{};
    // Host-written, GPU-read every frame: lands in BAR/ReBAR memory where the GPU has it.
    f.drawBuffer = createBuffer(
        capacity * sizeof(DrawData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, &info);
    f.drawData = static_cast<DrawData*>(info.pMappedData);
    f.drawAddress = bufferAddress(f.drawBuffer.buffer);
    f.drawCapacity = capacity;
}

void VulkanRenderer::Impl::ensureLayerCapacity(Frame& f, uint32_t count) {
    if (count <= f.layerCapacity) return;
    uint32_t capacity = std::max<uint32_t>(f.layerCapacity, 256);
    while (capacity < count) capacity *= 2;
    if (f.layerBuffer.buffer) vmaDestroyBuffer(context->allocator(), f.layerBuffer.buffer, f.layerBuffer.allocation);
    VmaAllocationInfo info{};
    f.layerBuffer = createBuffer(
        capacity * sizeof(LayerData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, &info);
    f.layerData = static_cast<LayerData*>(info.pMappedData);
    f.layerAddress = bufferAddress(f.layerBuffer.buffer);
    f.layerCapacity = capacity;
}

void VulkanRenderer::Impl::ensureProbeCapacity(Frame& f, uint32_t count) {
    if (count <= f.probeCapacity) return;
    uint32_t capacity = std::max<uint32_t>(f.probeCapacity, 256);
    while (capacity < count) capacity *= 2;
    if (f.probeBuffer.buffer) vmaDestroyBuffer(context->allocator(), f.probeBuffer.buffer, f.probeBuffer.allocation);
    VmaAllocationInfo info{};
    f.probeBuffer = createBuffer(
        capacity * sizeof(ProbeData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, &info);
    f.probeData = static_cast<ProbeData*>(info.pMappedData);
    f.probeAddress = bufferAddress(f.probeBuffer.buffer);
    f.probeCapacity = capacity;
}

void VulkanRenderer::Impl::createRenderFinishedSemaphores() {
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    renderFinished.resize(swapchain->imageCount());
    for (VkSemaphore& s : renderFinished) {
        vk::check(vkCreateSemaphore(context->device(), &semInfo, nullptr, &s), "vkCreateSemaphore");
    }
}

void VulkanRenderer::Impl::destroyRenderFinishedSemaphores() {
    for (VkSemaphore s : renderFinished) vkDestroySemaphore(context->device(), s, nullptr);
    renderFinished.clear();
}

void VulkanRenderer::Impl::createBindless() {
    VkDevice device = context->device();
    const uint32_t capacity = context->maxBindlessTextures();

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = std::min(16.0f, context->maxAnisotropy());
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    vk::check(vkCreateSampler(device, &samplerInfo, nullptr, &linearSampler), "vkCreateSampler");

    // Shadow maps are read with hardware depth comparison (bilinear PCF per tap).
    // Reverse-Z: a surface is lit when its depth is at least the map's (nothing nearer).
    VkSamplerCreateInfo shadowInfo{};
    shadowInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    shadowInfo.magFilter = VK_FILTER_LINEAR;
    shadowInfo.minFilter = VK_FILTER_LINEAR;
    shadowInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    shadowInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    shadowInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    shadowInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    shadowInfo.compareEnable = VK_TRUE;
    shadowInfo.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    shadowInfo.maxLod = 0.0f;
    vk::check(vkCreateSampler(device, &shadowInfo, nullptr, &shadowSampler), "vkCreateSampler");

    VkDescriptorSetLayoutBinding bindings[8]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = bindlessSlots();
    bindings[0].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[1].pImmutableSamplers = &linearSampler;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[2].pImmutableSamplers = &shadowSampler;
    // Image-based lighting's cubes (RendererEnvironment.cpp): environment, specular,
    // irradiance. Written once when they're made; their images are rebuilt in place.
    for (uint32_t i = 3; i < 6; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_ALL;
    }
    static_assert(kEnvironmentCubes == 3, "one binding per cube");
    // Material samplers (G02, RendererMaterials.cpp), two banks.
    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[6].descriptorCount = kMaxSamplers * kSamplerBanks;
    bindings[6].stageFlags = VK_SHADER_STAGE_ALL;
    // Local reflection probes' cubes (G07, RendererReflections.cpp), out of the same budget.
    bindings[7].binding = 7;
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[7].descriptorCount = kReflectionCubeSlots;
    bindings[7].stageFlags = VK_SHADER_STAGE_ALL;

    // Partially bound: unused slots may be empty. Update-after-bind + unused-while-pending:
    // new textures can be added while frames using the table are still in flight.
    const VkDescriptorBindingFlags bindingFlags[8] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
        0,
        0,
        0,
        0,
        0,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
    };
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount = 8;
    flagsInfo.pBindingFlags = bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext = &flagsInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 8;
    layoutInfo.pBindings = bindings;
    vk::check(
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &bindlessLayout), "vkCreateDescriptorSetLayout");

    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 2 + kMaxSamplers * kSamplerBanks},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    vk::check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &bindlessPool), "vkCreateDescriptorPool");

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = bindlessPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &bindlessLayout;
    vk::check(vkAllocateDescriptorSets(device, &allocInfo, &bindlessSet), "vkAllocateDescriptorSets");

    // The default material sampler, SamplerHandle 0: the same as linearSampler.
    samplerObjects[0][0] = makeSampler(SamplerDesc{});
    writeSamplerDescriptor(0, samplerObjects[0][0]);
}

void VulkanRenderer::Impl::createPipelines() {
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = sizeof(DrawPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &bindlessLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    vk::check(
        vkCreatePipelineLayout(context->device(), &layoutInfo, nullptr, &pipelineLayout), "vkCreatePipelineLayout");

    vk::GraphicsPipelineDesc desc;
    desc.vertexShader = shaderPath("mesh.vert");
    desc.fragmentShader = shaderPath("mesh.frag");
    desc.colorFormat = kSceneColorFormat;
    desc.depthFormat = kDepthFormat;
    desc.layout = pipelineLayout;
    meshPipeline = vk::createGraphicsPipeline(context->device(), desc);
    desc.cullMode = VK_CULL_MODE_NONE;
    meshDoubleSidedPipeline = vk::createGraphicsPipeline(context->device(), desc);
    desc.alphaBlend = true;
    meshBlendDoubleSidedPipeline = vk::createGraphicsPipeline(context->device(), desc);
    desc.cullMode = VK_CULL_MODE_BACK_BIT;
    meshBlendPipeline = vk::createGraphicsPipeline(context->device(), desc);
    desc.frontFace = VK_FRONT_FACE_CLOCKWISE;
    for (int i = 0; i < 4; ++i) {
        desc.alphaBlend = (i & 2) != 0;
        desc.cullMode = (i & 1) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        mirroredPipelines[i] = vk::createGraphicsPipeline(context->device(), desc);
    }
    // Solid surfaces (opaque, no alpha test) get mesh.frag without its discard, which lets
    // the GPU reject hidden fragments before shading them.
    const VkBool32 noAlphaTest = VK_FALSE;
    const VkSpecializationMapEntry alphaTestEntry{0, 0, sizeof(VkBool32)};
    const VkSpecializationInfo solid{1, &alphaTestEntry, sizeof(noAlphaTest), &noAlphaTest};
    desc.fragmentSpecialization = &solid;
    desc.alphaBlend = false;
    for (int i = 0; i < 4; ++i) {
        desc.frontFace = (i & 2) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        desc.cullMode = (i & 1) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        solidPipelines[i] = vk::createGraphicsPipeline(context->device(), desc);
    }

    // The editor grid: a transparent quad on the ground, depth-tested against the
    // scene but not writing depth, drawn after everything opaque.
    VkPushConstantRange gridRange{};
    gridRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    gridRange.size = sizeof(GridPushConstants);
    VkPipelineLayoutCreateInfo gridLayoutInfo{};
    gridLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    gridLayoutInfo.pushConstantRangeCount = 1;
    gridLayoutInfo.pPushConstantRanges = &gridRange;
    vk::check(
        vkCreatePipelineLayout(context->device(), &gridLayoutInfo, nullptr, &gridLayout), "vkCreatePipelineLayout");
    vk::GraphicsPipelineDesc gridDesc;
    gridDesc.vertexShader = shaderPath("grid.vert");
    gridDesc.fragmentShader = shaderPath("grid.frag");
    gridDesc.colorFormat = kSceneColorFormat;
    gridDesc.depthFormat = kDepthFormat;
    gridDesc.layout = gridLayout;
    gridDesc.cullMode = VK_CULL_MODE_NONE;   // seen from below too
    gridDesc.depthWrite = false;
    gridDesc.alphaBlend = true;
    gridPipeline = vk::createGraphicsPipeline(context->device(), gridDesc);

    // Tonemapping: one fullscreen triangle from the HDR scene to the display image.
    VkPushConstantRange tonemapRange{};
    tonemapRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    tonemapRange.size = sizeof(TonemapPushConstants);
    VkPipelineLayoutCreateInfo tonemapLayoutInfo{};
    tonemapLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    tonemapLayoutInfo.setLayoutCount = 1;
    tonemapLayoutInfo.pSetLayouts = &bindlessLayout;
    tonemapLayoutInfo.pushConstantRangeCount = 1;
    tonemapLayoutInfo.pPushConstantRanges = &tonemapRange;
    vk::check(
        vkCreatePipelineLayout(context->device(), &tonemapLayoutInfo, nullptr, &tonemapLayout),
        "vkCreatePipelineLayout");
    vk::GraphicsPipelineDesc tonemapDesc;
    tonemapDesc.vertexShader = shaderPath("tonemap.vert");
    tonemapDesc.fragmentShader = shaderPath("tonemap.frag");
    tonemapDesc.colorFormat = kDisplayFormat;
    tonemapDesc.layout = tonemapLayout;
    tonemapDesc.cullMode = VK_CULL_MODE_NONE;
    tonemapPipeline = vk::createGraphicsPipeline(context->device(), tonemapDesc);

    // Shadow maps: depth only, both faces (single-sided planes still cast), bias per map.
    VkPushConstantRange shadowRange{};
    shadowRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    shadowRange.size = sizeof(ShadowPushConstants);
    VkPipelineLayoutCreateInfo shadowLayoutInfo{};
    shadowLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    shadowLayoutInfo.setLayoutCount = 1;
    shadowLayoutInfo.pSetLayouts = &bindlessLayout;
    shadowLayoutInfo.pushConstantRangeCount = 1;
    shadowLayoutInfo.pPushConstantRanges = &shadowRange;
    vk::check(
        vkCreatePipelineLayout(context->device(), &shadowLayoutInfo, nullptr, &shadowLayout), "vkCreatePipelineLayout");
    vk::GraphicsPipelineDesc shadowDesc;
    shadowDesc.vertexShader = shaderPath("shadow.vert");
    shadowDesc.fragmentShader = shaderPath("shadow.frag");
    shadowDesc.depthFormat = kShadowFormat;
    shadowDesc.layout = shadowLayout;
    shadowDesc.cullMode = VK_CULL_MODE_NONE;
    shadowDesc.dynamicDepthBias = true;
    shadowPipeline = vk::createGraphicsPipeline(context->device(), shadowDesc);
    // Most casters only need depth: no fragment shader keeps early depth testing and skips
    // the shading work entirely. shadowPipeline is for alpha-tested ones.
    shadowDesc.fragmentShader.clear();
    shadowDepthOnlyPipeline = vk::createGraphicsPipeline(context->device(), shadowDesc);
    createParticlePipelines();
    createSkinningPipeline();
    createBloomPipeline();
    createAntiAliasingPipeline();
    createAmbientOcclusionPipeline();
    createDecalPipeline();   // after it: shares its sampler

    createUiPipeline();
}

}   // namespace shockwave
