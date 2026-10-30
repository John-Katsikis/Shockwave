// The surface prepass (G03) and screen-space ambient occlusion (G04,
// docs/GraphicsImplementation.md 7 and 8).
//
// With ambient_occlusion on, the frame goes shadows, surface prepass, ambient occlusion,
// scene. The prepass draws every opaque and alpha-tested surface with the scene's own
// vertex shader and alpha test, writing depth and the geometric normal in view space
// (octahedral, shaders/prepass.frag). Shaders/ssao.comp then works out, at half
// resolution, how much of the hemisphere above each surface the depth buffer shows to be
// blocked nearby, and blurs it twice (across, then down) without crossing edges. The scene
// shades opaque surfaces over the prepass's depth, testing for EQUAL without writing, and
// mesh.frag upsamples the occlusion for the light from the surroundings alone.
//
// Off, there's no prepass, no images and no pipelines in use: the frame is as it was. The
// images follow the setting at a frame boundary like bloom's, the old ones retired.
#include "RendererImpl.h"

namespace shockwave {

namespace {

constexpr VkFormat kAoFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Must match the push constants in shaders/ssao.comp.
struct AoPushConstants {
    VkDeviceAddress frame;
    uint32_t mode;   // 0 occlusion, 1 blur across, 2 blur down
    uint32_t samples;
    glm::uvec2 fullSize;
    glm::uvec2 halfSize;
};
static_assert(sizeof(AoPushConstants) == 32, "AoPushConstants must match shaders/ssao.comp");

VkExtent2D halfExtent(VkExtent2D full) { return {(full.width + 1) / 2, (full.height + 1) / 2}; }

}   // namespace

void VulkanRenderer::Impl::createAmbientOcclusionPipeline() {
    VkDevice device = context->device();
    // Two components of 16-bit signed normalized hold an octahedral normal exactly enough;
    // where that can't be rendered to, half floats do.
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(context->physicalDevice(), VK_FORMAT_R16G16_SNORM, &props);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    aoNormalFormat = (props.optimalTilingFeatures & need) == need ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_SFLOAT;

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk::check(vkCreateSampler(device, &samplerInfo, nullptr, &aoSampler), "vkCreateSampler");

    // Depth and normals (sampled), then the blur's input and every step's output (storage).
    VkDescriptorSetLayoutBinding bindings[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[i].pImmutableSamplers = i < 2 ? &aoSampler : nullptr;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings = bindings;
    vk::check(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &aoSetLayout), "vkCreateDescriptorSetLayout");

    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AoPushConstants)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &aoSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &range;
    vk::check(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &aoLayout), "vkCreatePipelineLayout");

    VkShaderModule module = vk::loadShaderModule(device, shaderPath("ssao.comp"));
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = aoLayout;
    const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &aoPipeline);
    vkDestroyShaderModule(device, module, nullptr);
    vk::check(result, "vkCreateComputePipelines");

    // The prepass: mesh.vert with the scene's culling and alpha test, into depth and normals.
    vk::GraphicsPipelineDesc desc;
    desc.vertexShader = shaderPath("mesh.vert");
    desc.fragmentShader = shaderPath("prepass.frag");
    desc.colorFormat = aoNormalFormat;
    desc.depthFormat = kDepthFormat;
    desc.layout = pipelineLayout;
    // The shading over it: mesh.frag, depth EQUAL without writes, reading the occlusion.
    vk::GraphicsPipelineDesc shade;
    shade.vertexShader = shaderPath("mesh.vert");
    shade.fragmentShader = shaderPath("mesh.frag");
    shade.colorFormat = kSceneColorFormat;
    shade.depthFormat = kDepthFormat;
    shade.layout = pipelineLayout;
    shade.depthWrite = false;
    shade.depthCompare = VK_COMPARE_OP_EQUAL;
    const VkSpecializationMapEntry entries[2] = {{0, 0, sizeof(VkBool32)}, {1, sizeof(VkBool32), sizeof(VkBool32)}};
    for (uint32_t i = 0; i < 8; ++i) {
        const VkBool32 constants[2] = {(i & 4) ? VK_TRUE : VK_FALSE, VK_TRUE};   // alpha test, receives AO
        const VkSpecializationInfo prepassSpec{1, entries, sizeof(VkBool32), constants};
        const VkSpecializationInfo shadeSpec{2, entries, sizeof(constants), constants};
        desc.frontFace = shade.frontFace = (i & 2) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        desc.cullMode = shade.cullMode = (i & 1) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        desc.fragmentSpecialization = &prepassSpec;
        shade.fragmentSpecialization = &shadeSpec;
        prepassPipelines[i] = vk::createGraphicsPipeline(device, desc);
        depthEqualPipelines[i] = vk::createGraphicsPipeline(device, shade);
    }
}

void VulkanRenderer::Impl::destroyAmbientOcclusionPipeline() {
    VkDevice device = context->device();
    for (VkPipeline pipeline : prepassPipelines) vkDestroyPipeline(device, pipeline, nullptr);
    for (VkPipeline pipeline : depthEqualPipelines) vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipeline(device, aoPipeline, nullptr);
    vkDestroyPipelineLayout(device, aoLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, aoSetLayout, nullptr);
    vkDestroySampler(device, aoSampler, nullptr);
}

AmbientOcclusion VulkanRenderer::Impl::effectiveAmbientOcclusion() const {
    // The debug view shows occlusion even with it off, at medium.
    if (debugView == DebugView::AmbientOcclusion && ambientOcclusion == AmbientOcclusion::Off)
        return AmbientOcclusion::Medium;
    return ambientOcclusion;
}

bool VulkanRenderer::Impl::createAoTargets() {
    VkDevice device = context->device();
    int free = -1;
    for (int i = 0; i < 2 && free < 0; ++i)
        if (!aoSlotBusy[i] && !(aoTargets.normal.image && aoSlots[i] == aoTargets.slot)) free = i;
    if (free < 0) return false;

    AoTargets targets;
    targets.slot = aoSlots[free];
    const VkExtent2D full = sceneColor.extent, half = halfExtent(full);
    targets.normal = createImage(
        full, aoNormalFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, 1, true);
    for (vk::Image& image : targets.ao)
        image = createImage(
            half, kAoFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1,
            true);

    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 3;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    vk::check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &targets.pool), "vkCreateDescriptorPool");
    // Each step reads depth and normals and writes one half-resolution image from the other.
    const int steps[3][2] = {{1, 0}, {0, 1}, {1, 0}};   // source, target
    for (int i = 0; i < 3; ++i) {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = targets.pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &aoSetLayout;
        vk::check(vkAllocateDescriptorSets(device, &allocInfo, &targets.sets[i]), "vkAllocateDescriptorSets");
        const VkDescriptorImageInfo images[4] = {
            {VK_NULL_HANDLE, sceneDepth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, targets.normal.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, targets.ao[steps[i][0]].view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, targets.ao[steps[i][1]].view, VK_IMAGE_LAYOUT_GENERAL},
        };
        VkWriteDescriptorSet writes[4]{};
        for (uint32_t b = 0; b < 4; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = targets.sets[i];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType =
                b < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[b].pImageInfo = &images[b];
        }
        vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
    }
    // mesh.frag reads the result through the bindless table, from a slot no frame in
    // flight uses.
    {
        std::lock_guard lock(descriptorMutex);
        VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, targets.ao[0].view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = bindlessSet;
        write.dstBinding = 0;
        write.dstArrayElement = targets.slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
    if (aoTargets.normal.image) {
        const int slot = aoTargets.slot == aoSlots[0] ? 0 : 1;
        aoSlotBusy[slot] = true;
        retire([this, old = aoTargets, slot]() mutable {
            destroyAoTargets(old);
            aoSlotBusy[slot] = false;
        });
    }
    aoTargets = targets;
    aoNormalState = {};   // a new image starts undefined
    return true;
}

void VulkanRenderer::Impl::destroyAoTargets(AoTargets& targets) {
    if (targets.pool) vkDestroyDescriptorPool(context->device(), targets.pool, nullptr);
    destroyImage(targets.normal);
    for (vk::Image& image : targets.ao) destroyImage(image);
    targets = {};
}

void VulkanRenderer::Impl::updateAoTargets() {
    const bool want = effectiveAmbientOcclusion() != AmbientOcclusion::Off;
    if (want == prepassActive()) return;
    if (!want) {
        const int slot = aoTargets.slot == aoSlots[0] ? 0 : 1;
        aoSlotBusy[slot] = true;
        retire([this, old = aoTargets, slot]() mutable {
            destroyAoTargets(old);
            aoSlotBusy[slot] = false;
        });
        aoTargets = {};
        return;
    }
    createAoTargets();   // false: both slots in flight; the next frame tries again
}

void VulkanRenderer::Impl::recordPrepass(VkCommandBuffer cmd, Frame& f) {
    VkRenderingAttachmentInfo normalAttachment{};
    normalAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    normalAttachment.imageView = aoTargets.normal.view;
    normalAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    normalAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    normalAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    normalAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = sceneDepth.view;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;   // the scene tests against it
    depthAttachment.clearValue.depthStencil = {0.0f, 0};   // reverse-Z: 0 is infinitely far

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, sceneColor.extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &normalAttachment;
    rendering.pDepthAttachment = &depthAttachment;
    vkCmdBeginRendering(cmd, &rendering);
    setSceneViewport(cmd);
    if (!gatheredDraws.empty()) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &bindlessSet, 0, nullptr);
        DrawPushConstants push{f.drawAddress, f.frameAddress};
        vkCmdPushConstants(
            cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        VkBuffer boundIndices = VK_NULL_HANDLE;
        VkPipeline boundPipeline = VK_NULL_HANDLE;
        for (const DrawCommand& draw : gatheredDraws) {
            if (!draw.visible || draw.blend) continue;   // blended surfaces aren't in it
            const VkPipeline pipeline = prepassPipelines[prepassIndex(draw)];
            if (pipeline != boundPipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                boundPipeline = pipeline;
            }
            if (draw.indexBuffer != boundIndices) {
                vkCmdBindIndexBuffer(cmd, draw.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                boundIndices = draw.indexBuffer;
            }
            vkCmdDrawIndexed(cmd, draw.indexCount, 1, draw.firstIndex, 0, draw.drawIndex);
        }
    }
    vkCmdEndRendering(cmd);
}

void VulkanRenderer::Impl::recordAmbientOcclusion(VkCommandBuffer cmd, Frame& f) {
    auto barrier = [&](const vk::Image& image, VkImageLayout from, VkPipelineStageFlags2 srcStage,
                       VkPipelineStageFlags2 dstStage) {
        transitionImage(
            cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, from, VK_IMAGE_LAYOUT_GENERAL, srcStage,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, dstStage,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    };
    // Last frame's contents aren't needed; wait for last frame's scene to finish reading.
    for (const vk::Image& image : aoTargets.ao)
        barrier(
            image, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, aoPipeline);
    const VkExtent2D full = sceneColor.extent, half = halfExtent(full);
    const int targets[3] = {0, 1, 0};
    for (uint32_t mode = 0; mode < 3; ++mode) {
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_COMPUTE, aoLayout, 0, 1, &aoTargets.sets[mode], 0, nullptr);
        const AoPushConstants push{
            f.frameAddress, mode, ambientOcclusionSamples(effectiveAmbientOcclusion()),
            glm::uvec2(full.width, full.height), glm::uvec2(half.width, half.height)};
        vkCmdPushConstants(cmd, aoLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (half.width + 7) / 8, (half.height + 7) / 8, 1);
        // The next step reads what this one wrote; after the last, the scene does.
        barrier(
            aoTargets.ao[targets[mode]], VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            mode < 2 ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    }
}

}   // namespace shockwave
