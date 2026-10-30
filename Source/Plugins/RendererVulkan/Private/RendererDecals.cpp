// Projected decals (G08, docs/GraphicsImplementation.md 12; Renderer/Decals.h).
//
// While decals_quality is on and the scene has decals, the frame goes shadows, surface
// prepass (which also writes each surface's receiver layer bits), decals, scene. The decal
// pass draws each selected decal's box, back faces only and with no depth test, into two
// RGBA8 buffers cleared to zero coverage: premultiplied colour, and premultiplied roughness
// and metallic. shaders/decal.frag reconstructs the visible surface from the prepass's
// depth, keeps it if it's inside the box, on a receiving layer and facing the decal, and
// blends "over" in order. Opaque shading reads both buffers once, before lighting
// (mesh.frag). Blended surfaces aren't in the prepass and never take decals.
//
// Off, or with no decals, there's no layer target, no buffers and no decal pass; and the
// prepass runs only if ambient occlusion wants it. The images follow the setting at a
// frame boundary like ambient occlusion's, the old ones retired.
#include "RendererImpl.h"

#include <Core/Log.h>

namespace shockwave {

namespace {

constexpr VkFormat kDecalBufferFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDecalLayerFormat = VK_FORMAT_R8_UINT;

// Must match the push constants in shaders/decal.vert and decal.frag.
struct DecalPushConstants {
    VkDeviceAddress frame;
    VkDeviceAddress decals;
};

}   // namespace

void VulkanRenderer::Impl::createDecalPipeline() {
    VkDevice device = context->device();
    // The prepass's depth, normals and receiver layers, read with the nearest-texel sampler.
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[i].pImmutableSamplers = &aoSampler;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    vk::check(
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &decalSetLayout), "vkCreateDescriptorSetLayout");

    // Set 0 is the bindless table (the decals' textures), set 1 the prepass's images.
    const VkDescriptorSetLayout sets[2] = {bindlessLayout, decalSetLayout};
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DecalPushConstants)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 2;
    pipelineLayoutInfo.pSetLayouts = sets;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &range;
    vk::check(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &decalLayout), "vkCreatePipelineLayout");

    vk::GraphicsPipelineDesc desc;
    desc.vertexShader = shaderPath("decal.vert");
    desc.fragmentShader = shaderPath("decal.frag");
    desc.colorFormat = kDecalBufferFormat;
    desc.secondColorFormat = kDecalBufferFormat;
    desc.layout = decalLayout;
    desc.premultipliedAlpha = true;   // "over", in the order drawn
    // Back faces only, so each pixel is shaded once whether the camera is outside the box
    // or inside it (where there are no front faces to draw). The cube is counterclockwise
    // seen from outside, like every mesh, and meshes cull with the same front face.
    desc.cullMode = VK_CULL_MODE_FRONT_BIT;
    desc.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    decalPipeline = vk::createGraphicsPipeline(device, desc);
}

void VulkanRenderer::Impl::destroyDecalPipeline() {
    VkDevice device = context->device();
    vkDestroyPipeline(device, decalPipeline, nullptr);
    vkDestroyPipelineLayout(device, decalLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, decalSetLayout, nullptr);
}

bool VulkanRenderer::Impl::decalsWanted() const { return decalQuality != DecalQuality::Off && lastSubmittedDecals > 0; }

bool VulkanRenderer::Impl::createDecalTargets() {
    if (!prepassActive()) return false;   // the prepass's images come first (updateAoTargets)
    VkDevice device = context->device();
    int free = -1;
    for (int i = 0; i < 2 && free < 0; ++i)
        if (!decalSlotBusy[i] && !(decalsActive() && decalTargets.pair == i)) free = i;
    if (free < 0) return false;

    DecalTargets targets;
    targets.pair = free;
    const VkExtent2D full = sceneColor.extent;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    targets.layers = createImage(full, kDecalLayerFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT, 1, true);
    targets.albedo = createImage(full, kDecalBufferFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT, 1, true);
    targets.material = createImage(full, kDecalBufferFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT, 1, true);

    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    vk::check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &targets.pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = targets.pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &decalSetLayout;
    vk::check(vkAllocateDescriptorSets(device, &allocInfo, &targets.set), "vkAllocateDescriptorSets");
    // Depth is read from the read-only depth layout it's in for the scene pass.
    const VkDescriptorImageInfo images[3] = {
        {VK_NULL_HANDLE, sceneDepth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, aoTargets.normal.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, targets.layers.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t b = 0; b < 3; ++b) {
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = targets.set;
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[b].pImageInfo = &images[b];
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
    // mesh.frag reads the two buffers through the bindless table, from slots no frame in
    // flight uses.
    {
        std::lock_guard lock(descriptorMutex);
        const VkDescriptorImageInfo buffers[2] = {
            {VK_NULL_HANDLE, targets.albedo.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, targets.material.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        };
        VkWriteDescriptorSet slotWrites[2]{};
        for (int i = 0; i < 2; ++i) {
            slotWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            slotWrites[i].dstSet = bindlessSet;
            slotWrites[i].dstBinding = 0;
            slotWrites[i].dstArrayElement = decalSlots[free][i];
            slotWrites[i].descriptorCount = 1;
            slotWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            slotWrites[i].pImageInfo = &buffers[i];
        }
        vkUpdateDescriptorSets(device, 2, slotWrites, 0, nullptr);
    }
    if (decalsActive()) {
        const int pair = decalTargets.pair;
        decalSlotBusy[pair] = true;
        retire([this, old = decalTargets, pair]() mutable {
            destroyDecalTargets(old);
            decalSlotBusy[pair] = false;
        });
    }
    decalTargets = targets;
    decalLayerState = decalAlbedoState = decalMaterialState = {};   // new images start undefined
    return true;
}

void VulkanRenderer::Impl::destroyDecalTargets(DecalTargets& targets) {
    if (&targets == &decalTargets) decalNormalView = VK_NULL_HANDLE;
    if (targets.pool) vkDestroyDescriptorPool(context->device(), targets.pool, nullptr);
    destroyImage(targets.layers);
    destroyImage(targets.albedo);
    destroyImage(targets.material);
    targets = {};
}

void VulkanRenderer::Impl::updateDecalTargets() {
    // The prepass's normal image may have been replaced (a resize, or ambient occlusion
    // turned on or off): the decal set reads it, so the targets follow.
    const bool want = decalsWanted() && prepassActive();
    if (!want) {
        if (!decalsActive()) return;
        const int pair = decalTargets.pair;
        decalSlotBusy[pair] = true;
        retire([this, old = decalTargets, pair]() mutable {
            destroyDecalTargets(old);
            decalSlotBusy[pair] = false;
        });
        decalTargets = {};
        return;
    }
    if (decalsActive() && decalTargets.albedo.extent.width == sceneColor.extent.width &&
        decalTargets.albedo.extent.height == sceneColor.extent.height && decalNormalView == aoTargets.normal.view)
        return;
    if (createDecalTargets()) decalNormalView = aoTargets.normal.view;   // false: both pairs in flight; next frame
}

void VulkanRenderer::Impl::drawDecals(std::span<const DecalDesc> decals) {
    std::lock_guard lock(decalMutex);
    const size_t room = kMaxSubmittedDecals - std::min<size_t>(submittedDecals.size(), kMaxSubmittedDecals);
    const size_t take = std::min(room, decals.size());
    submittedDecals.insert(submittedDecals.end(), decals.begin(), decals.begin() + take);
    droppedDecals += uint32_t(decals.size() - take);
}

void VulkanRenderer::Impl::prepareDecals() {
    frameDecals.clear();
    std::vector<DecalDesc> all;
    {
        std::lock_guard lock(decalMutex);
        all.swap(submittedDecals);
    }
    DecalStats stats;
    stats.submitted = uint32_t(all.size()) + droppedDecals;
    droppedDecals = 0;
    lastSubmittedDecals = stats.submitted;
    if (decalQuality == DecalQuality::Off || all.empty()) {
        lastDecalStats = stats;
        return;
    }
    const glm::mat4 viewProjection = projection * view;
    std::vector<DecalDesc> visible;
    visible.reserve(all.size());
    for (const DecalDesc& d : all)
        if (decalMayBeVisible(d, viewProjection)) visible.push_back(d);
    stats.visible = uint32_t(visible.size());
    const uint32_t budget = std::min(decalBudget(decalQuality), kMaxDrawnDecals);
    const std::vector<uint32_t> order = selectDecals(visible, cameraPosition, budget);
    stats.overBudget = stats.visible - uint32_t(order.size());
    if (decalsActive()) {
        frameDecals.reserve(order.size());
        for (uint32_t i : order) {
            const DecalDesc& d = visible[i];
            DecalData g{};
            g.boxToWorld = d.boxToWorld;
            g.worldToBox = glm::inverse(d.boxToWorld);
            g.color = glm::clamp(
                d.color, glm::vec4(0.0f),
                glm::vec4(
                    std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max(), 1.0f));
            g.facing = decalFacing(d);
            g.fadeStartCos = std::cos(glm::radians(std::clamp(d.angleFadeDegrees, 0.0f, 89.0f)));
            g.texture = textureSlot(d.texture.id);
            g.channels = d.channels & (kDecalAlbedo | kDecalRoughnessMetallic);
            g.receiverMask = d.receiverMask & kDecalLayerMask;
            g.roughness = std::clamp(d.roughness, 0.0f, 1.0f);
            g.metallic = std::clamp(d.metallic, 0.0f, 1.0f);
            if (g.channels && g.receiverMask) frameDecals.push_back(g);
        }
    }
    stats.drawn = uint32_t(frameDecals.size());
    lastDecalStats = stats;
}

void VulkanRenderer::Impl::writeDecals(Frame& f) {
    if (frameDecals.empty()) return;
    std::memcpy(f.frameData + kDecalsOffset, frameDecals.data(), frameDecals.size() * sizeof(DecalData));
}

void VulkanRenderer::Impl::recordDecals(VkCommandBuffer cmd, Frame& f) {
    VkRenderingAttachmentInfo attachments[2]{};
    for (int i = 0; i < 2; ++i) {
        attachments[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attachments[i].imageView = i == 0 ? decalTargets.albedo.view : decalTargets.material.view;
        attachments[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;   // zero coverage everywhere
        attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[i].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    }
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, sceneColor.extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 2;
    rendering.pColorAttachments = attachments;
    vkCmdBeginRendering(cmd, &rendering);
    setSceneViewport(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, decalPipeline);
    const VkDescriptorSet sets[2] = {bindlessSet, decalTargets.set};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, decalLayout, 0, 2, sets, 0, nullptr);
    const DecalPushConstants push{f.frameAddress, f.frameAddress + kDecalsOffset};
    vkCmdPushConstants(
        cmd, decalLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    // One instance per decal, in order: rasterization keeps instance order, so the blend
    // composes them one over the next.
    vkCmdDraw(cmd, 36, uint32_t(frameDecals.size()), 0, 0);
    vkCmdEndRendering(cmd);
}

}   // namespace shockwave
