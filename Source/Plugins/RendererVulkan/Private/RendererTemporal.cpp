// Motion vectors and temporal anti-aliasing (G09, G10; docs/TemporalAntiAliasing.md, the
// rules in Renderer/Temporal.h).
//
// While anti_aliasing is taa, a lit view with a RenderViewId jitters its camera by a
// centred Halton(2, 3) sub-pixel offset each frame (setCamera, jitterCamera). The surface
// prepass also writes each surface's motion since last frame, from the draw's previous
// pose: its model matrix last frame, found by RenderInstanceKey, and for skinned draws the
// vertices skinned again with last frame's joints. After the scene and particles, blended
// draws and particles write the reactive mask, and Shaders/taa.comp blends the scene with
// its history into the other history image, which is copied back into the scene before
// bloom and tone mapping. Game UI is drawn after all of it and never enters history.
//
// History starts afresh (Temporal.h's historyReset) on a new view, a resize, a projection
// change, Renderer::cutCamera, a change of anti-aliasing or debug view, and the first frame.
//
// Off, there's no jitter, no targets, no passes and no pipelines in use; the motion and
// reactive debug views make the targets for themselves. The images follow the settings at
// a frame boundary like the others, the old ones retired.
#include "RendererImpl.h"

#include <Core/Profile.h>

namespace shockwave {

namespace {

constexpr VkFormat kReactiveFormat = VK_FORMAT_R8_UNORM;
constexpr VkFormat kHistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kHistoryDistanceFormat = VK_FORMAT_R32_SFLOAT;

// Must match the push constants in shaders/taa.comp.
struct TaaPushConstants {
    glm::mat4 reprojection;
    VkDeviceAddress frame;
    glm::vec2 previousJitter;
    uint32_t mode;   // 0 resolve, 1 motion view, 2 reactive view
    uint32_t historyValid;
    uint32_t orthographic;
    uint32_t pad[3];
};
static_assert(sizeof(TaaPushConstants) == 104, "TaaPushConstants must match shaders/taa.comp");

bool orthographicProjection(const glm::mat4& projection) { return std::abs(projection[2][3]) < 1e-6f; }

}   // namespace

void VulkanRenderer::Impl::createTemporalPipelines() {
    VkDevice device = context->device();
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk::check(vkCreateSampler(device, &samplerInfo, nullptr, &temporalNearestSampler), "vkCreateSampler");
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    vk::check(vkCreateSampler(device, &samplerInfo, nullptr, &temporalLinearSampler), "vkCreateSampler");

    // Scene colour, depth, motion, reactive, history colour (linear) and distance, sampled;
    // then the history this frame writes, colour and distance.
    VkDescriptorSetLayoutBinding bindings[8]{};
    for (uint32_t i = 0; i < 8; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType =
            i < 6 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[i].pImmutableSamplers = i == 4 ? &temporalLinearSampler : i < 6 ? &temporalNearestSampler : nullptr;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 8;
    layoutInfo.pBindings = bindings;
    vk::check(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &taaSetLayout), "vkCreateDescriptorSetLayout");

    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TaaPushConstants)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &taaSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &range;
    vk::check(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &taaLayout), "vkCreatePipelineLayout");

    VkShaderModule module = vk::loadShaderModule(device, shaderPath("taa.comp"));
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = taaLayout;
    const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &taaPipeline);
    vkDestroyShaderModule(device, module, nullptr);
    vk::check(result, "vkCreateComputePipelines");

    // Blended draws' coverage into the reactive mask: over the scene's depth, both faces.
    vk::GraphicsPipelineDesc desc;
    desc.vertexShader = shaderPath("mesh.vert");
    desc.fragmentShader = shaderPath("reactive.frag");
    desc.colorFormat = kReactiveFormat;
    desc.depthFormat = kDepthFormat;
    desc.layout = pipelineLayout;
    desc.cullMode = VK_CULL_MODE_NONE;
    desc.depthWrite = false;
    desc.maxBlend = true;
    reactiveMeshPipeline = vk::createGraphicsPipeline(device, desc);
    // The particles' is made with theirs (createParticlePipelines).
}

void VulkanRenderer::Impl::destroyTemporalPipelines() {
    VkDevice device = context->device();
    vkDestroyPipeline(device, reactiveMeshPipeline, nullptr);
    vkDestroyPipeline(device, taaPipeline, nullptr);
    vkDestroyPipelineLayout(device, taaLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, taaSetLayout, nullptr);
    vkDestroySampler(device, temporalNearestSampler, nullptr);
    vkDestroySampler(device, temporalLinearSampler, nullptr);
}

bool VulkanRenderer::Impl::temporalWanted() const {
    return antiAliasing == AntiAliasing::Taa || debugView == DebugView::Motion || debugView == DebugView::Reactive;
}

bool VulkanRenderer::Impl::taaThisFrame() const {
    // Debug views show raw values, like the tonemap pass and FXAA; a reflection capture and
    // a camera without a view have no history to blend.
    const bool lit =
        debugView == DebugView::Lit || debugView == DebugView::Lighting || debugView == DebugView::ShadowCascades;
    return lit && antiAliasing == AntiAliasing::Taa && temporalActive() && currentView.valid() && !captureRequested;
}

bool VulkanRenderer::Impl::createTemporalTargets() {
    VkDevice device = context->device();
    TemporalTargets targets;
    const VkExtent2D extent = sceneColor.extent;
    targets.motion = createImage(
        extent, kMotionFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, 1, true);
    targets.reactive = createImage(
        extent, kReactiveFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, 1, true);
    for (int i = 0; i < 2; ++i) {
        targets.color[i] = createImage(
            extent, kHistoryFormat,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, 1, true);
        targets.distance[i] = createImage(
            extent, kHistoryDistanceFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, 1, true);
    }

    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    vk::check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &targets.pool), "vkCreateDescriptorPool");
    for (int i = 0; i < 2; ++i) {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = targets.pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &taaSetLayout;
        vk::check(vkAllocateDescriptorSets(device, &allocInfo, &targets.sets[i]), "vkAllocateDescriptorSets");
        constexpr VkImageLayout read = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const VkDescriptorImageInfo images[8] = {
            {VK_NULL_HANDLE, sceneColor.view, read},
            {VK_NULL_HANDLE, sceneDepth.view, read},
            {VK_NULL_HANDLE, targets.motion.view, read},
            {VK_NULL_HANDLE, targets.reactive.view, read},
            {VK_NULL_HANDLE, targets.color[i ^ 1].view, read},
            {VK_NULL_HANDLE, targets.distance[i ^ 1].view, read},
            {VK_NULL_HANDLE, targets.color[i].view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, targets.distance[i].view, VK_IMAGE_LAYOUT_GENERAL},
        };
        VkWriteDescriptorSet writes[8]{};
        for (uint32_t b = 0; b < 8; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = targets.sets[i];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType =
                b < 6 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[b].pImageInfo = &images[b];
        }
        vkUpdateDescriptorSets(device, 8, writes, 0, nullptr);
    }
    temporalTargets = targets;
    // New images start undefined, and so does what they'd hold.
    motionState = reactiveState = {};
    for (int i = 0; i < 2; ++i) historyColorState[i] = historyDistanceState[i] = {};
    temporalView = {};
    return true;
}

void VulkanRenderer::Impl::destroyTemporalTargets(TemporalTargets& targets) {
    if (targets.pool) vkDestroyDescriptorPool(context->device(), targets.pool, nullptr);
    destroyImage(targets.motion);
    destroyImage(targets.reactive);
    for (int i = 0; i < 2; ++i) {
        destroyImage(targets.color[i]);
        destroyImage(targets.distance[i]);
    }
    targets = {};
}

void VulkanRenderer::Impl::updateTemporalTargets() {
    const bool want = temporalWanted();
    if (want == temporalActive()) return;
    if (want) {
        createTemporalTargets();
        return;
    }
    retire([this, old = temporalTargets]() mutable { destroyTemporalTargets(old); });
    temporalTargets = {};
    temporalView = {};
    previousPoses.clear();
    previousJoints.clear();
}

void VulkanRenderer::Impl::jitterCamera() {
    unjitteredProjection = projection;
    unjitteredViewProjection = viewProjection;
    viewFrustum = Frustum(unjitteredViewProjection);
    jittered = taaThisFrame();
    jitter = jittered ? taaJitter(static_cast<uint32_t>(frameNumber % kTaaJitterCycle)) : glm::vec2(0.0f);
    if (!jittered) return;
    const glm::vec2 extent(sceneColor.extent.width, sceneColor.extent.height);
    projection = jitterProjection(projection, jitter, extent);
    viewProjection = projection * view;
}

void VulkanRenderer::Impl::decideHistory() {
    previousSkins.clear();
    currentPoses.clear();
    currentJoints.clear();
    lastTemporalStats = {};
    lastTemporalStats.resets = temporalResets;
    const bool cut = cameraCut;
    cameraCut = false;
    temporalThisFrame = temporalActive() && prepassActive() && currentView.valid() && !captureRequested;
    if (!temporalThisFrame) {
        temporalView = {};
        previousPoses.clear();
        previousJoints.clear();
        historyValid = false;
        return;
    }
    TemporalViewState now;
    now.valid = true;
    now.view = currentView;
    now.extent = {sceneColor.extent.width, sceneColor.extent.height};
    now.orthographic = orthographicProjection(unjitteredProjection);
    const uint32_t mode = static_cast<uint32_t>(antiAliasing) | static_cast<uint32_t>(debugView) << 8;
    HistoryReset reset = historyReset(temporalView, now, cut);
    if (reset == HistoryReset::None && mode != temporalMode) reset = HistoryReset::ModeChanged;
    historyValid = reset == HistoryReset::None;
    if (!historyValid) {
        previousPoses.clear();
        previousJoints.clear();
        ++temporalResets;
    }
    temporalView = now;
    temporalMode = mode;
    lastTemporalStats.motion = true;
    lastTemporalStats.taa = taaThisFrame();
    lastTemporalStats.reset = reset;
    lastTemporalStats.resets = temporalResets;
    lastTemporalStats.jitter = jittered ? jitter : glm::vec2(0.0f);
}

void VulkanRenderer::Impl::prepareHistory() {
    if (!temporalThisFrame) return;
    SW_PROFILE_SCOPE("Prepare history");
    std::lock_guard lock(foreignDrawMutex);
    for (uint32_t i = 0; i < gatheredDraws.size(); ++i) {
        DrawCommand& draw = gatheredDraws[i];
        if (!draw.key.valid()) continue;
        std::span<const glm::mat4> joints;
        if (draw.skin != kNoSkin) {
            const DrawList& list = draw.skinList < drawLists.size() ? drawLists[draw.skinList] : foreignDraws;
            const SkinJob& job = list.skins[draw.skin];
            joints = std::span<const glm::mat4>(list.joints).subspan(job.firstJoint, job.jointCount);
        }
        // This frame's pose, for the next one. A key drawn twice keeps its first pose.
        const auto [current, added] = currentPoses.try_emplace(draw.key);
        if (added) {
            current->second.model = draw.data.model;
            current->second.firstJoint = static_cast<uint32_t>(currentJoints.size());
            current->second.jointCount = static_cast<uint32_t>(joints.size());
            currentJoints.insert(currentJoints.end(), joints.begin(), joints.end());
        }
        const auto previous = historyValid ? previousPoses.find(draw.key) : previousPoses.end();
        if (previous == previousPoses.end() || previous->second.jointCount != joints.size()) {
            ++lastTemporalStats.newDraws;
            continue;
        }
        // Levels of detail share the mesh's vertices, so a LOD change keeps its motion.
        draw.data.history = 1;
        draw.data.previousMvp = previousViewProjection * previous->second.model;
        if (!joints.empty()) previousSkins.push_back({i, previous->second.firstJoint, previous->second.jointCount});
        ++lastTemporalStats.historyDraws;
    }
}

void VulkanRenderer::Impl::finishHistory() {
    if (!temporalThisFrame) return;
    previousPoses.swap(currentPoses);
    previousJoints.swap(currentJoints);
    previousViewProjection = unjitteredViewProjection;
    previousJitter = jittered ? jitter : glm::vec2(0.0f);
    if (lastTemporalStats.taa || debugView == DebugView::Motion || debugView == DebugView::Reactive) historyWrite ^= 1u;
}

void VulkanRenderer::Impl::recordReactive(VkCommandBuffer cmd, Frame& f) {
    VkRenderingAttachmentInfo target{};
    target.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    target.imageView = temporalTargets.reactive.view;
    target.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    target.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;   // 0: nothing blended changed it
    target.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = sceneDepth.view;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, sceneColor.extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &target;
    rendering.pDepthAttachment = &depthAttachment;
    vkCmdBeginRendering(cmd, &rendering);
    setSceneViewport(cmd);

    // Blended draws: their coverage.
    bool bound = false;
    VkBuffer boundIndices = VK_NULL_HANDLE;
    for (const DrawCommand& draw : gatheredDraws) {
        if (!draw.visible || !draw.blend) continue;
        if (!bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, reactiveMeshPipeline);
            vkCmdBindDescriptorSets(
                cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &bindlessSet, 0, nullptr);
            DrawPushConstants push{f.drawAddress, f.frameAddress};
            vkCmdPushConstants(
                cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            bound = true;
        }
        if (draw.indexBuffer != boundIndices) {
            vkCmdBindIndexBuffer(cmd, draw.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            boundIndices = draw.indexBuffer;
        }
        vkCmdDrawIndexed(cmd, draw.indexCount, 1, draw.firstIndex, 0, draw.drawIndex);
    }
    // Particles: how much each changes what's behind it (shaders/particle.frag).
    if (!particleDraws.empty()) recordParticleReactive(cmd);
    vkCmdEndRendering(cmd);
}

void VulkanRenderer::Impl::recordTaa(VkCommandBuffer cmd, Frame& f) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, taaPipeline);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, taaLayout, 0, 1, &temporalTargets.sets[historyWrite], 0, nullptr);
    TaaPushConstants push{};
    push.reprojection = previousViewProjection * glm::inverse(unjitteredViewProjection);
    push.frame = f.frameAddress;
    push.previousJitter = previousJitter;
    push.mode = debugView == DebugView::Motion ? 1u : debugView == DebugView::Reactive ? 2u : 0u;
    push.historyValid = historyValid ? 1u : 0u;
    push.orthographic = orthographicProjection(unjitteredProjection) ? 1u : 0u;
    vkCmdPushConstants(cmd, taaLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    const VkExtent2D extent = sceneColor.extent;
    vkCmdDispatch(cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
}

void VulkanRenderer::Impl::recordTaaCopy(VkCommandBuffer cmd) {
    // The resolved image back into the scene, for bloom and tone mapping as before.
    VkImageCopy2 region{};
    region.sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2;
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {sceneColor.extent.width, sceneColor.extent.height, 1};
    VkCopyImageInfo2 copy{};
    copy.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2;
    copy.srcImage = temporalTargets.color[historyWrite].image;
    copy.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    copy.dstImage = sceneColor.image;
    copy.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    copy.regionCount = 1;
    copy.pRegions = &region;
    vkCmdCopyImage2(cmd, &copy);
}

}   // namespace shockwave
