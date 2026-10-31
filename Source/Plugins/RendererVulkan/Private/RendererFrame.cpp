#include "RendererImpl.h"

#include <Core/Log.h>
#include <Core/Profile.h>

#include <optional>

namespace shockwave {

void VulkanRenderer::Impl::drawMesh(
    MeshHandle handle, const glm::mat4& transform, TextureHandle texture, const glm::vec4& tint,
    const DrawMaterial& material, const RenderInstanceKey& key, std::span<const glm::mat4> joints) {
    if (handle.id >= kMaxMeshes || !meshReady[handle.id].load(std::memory_order_acquire)) return;
    const float determinant = glm::determinant(transform);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12f) return;
    const Mesh& mesh = meshes[handle.id];

    // The per-draw math runs here, on whichever thread submits the draw.
    DrawCommand command;
    command.data.mvp = viewProjection * transform;
    command.data.model = transform;
    command.data.normalMatrix = glm::transpose(glm::inverse(transform));
    command.data.roughness = material.roughness;
    command.data.metallic = material.metallic;
    command.data.emissive = material.emissive;
    // Texture ids become the bindless slots their images are in now (applyTextureSwaps).
    command.data.normalTexture = material.normalTexture.id ? textureSlot(material.normalTexture.id) : 0;
    command.data.metallicRoughnessTexture = textureSlot(material.metallicRoughnessTexture.id);
    command.data.emissiveTexture = textureSlot(material.emissiveTexture.id);
    command.data.occlusionTexture = material.occlusionTexture.id ? textureSlot(material.occlusionTexture.id) : 0;
    command.data.normalScale = material.normalScale;
    command.data.occlusionStrength = material.occlusionStrength;
    command.data.tint = tint;
    command.data.uvScale = material.uvScale;
    command.data.vertexBuffer = mesh.vertexAddress;
    command.data.uv1Buffer = mesh.uv1Address;
    command.data.textureIndex = textureSlot(texture.id);
    for (size_t i = 0; i < kMaterialSlots; ++i) {
        const TextureBinding& binding = material.bindings[i];
        SlotBinding& slot = command.data.slots[i];
        slot.offset = binding.transform.offset;
        slot.scale = binding.transform.scale;
        slot.rotation = binding.transform.rotation;
        slot.samplerAndSet = samplerSlot(binding.sampler) | (binding.uvSet == 1 ? kSlotUvSet1 : 0u);
    }
    // The vertex tangents follow UV0 as modelled. A normal map read through UV1, rotated or
    // mirrored UVs needs a tangent frame of its own UVs (shaders/mesh.frag).
    auto vertexTangents = [&](const TextureBinding& normal) {
        return normal.uvSet == 0 && normal.transform.rotation == 0.0f && normal.transform.scale.x > 0.0f &&
               normal.transform.scale.y > 0.0f && material.uvScale.x > 0.0f && material.uvScale.y > 0.0f;
    };
    command.data.flags = material.normalTexture.id && !vertexTangents(material.bindings[size_t(MaterialSlot::Normal)])
                             ? kDrawDerivativeTangents
                             : 0u;
    command.data.layers = 0;
    // Detail and second layers (A02); material_quality low leaves the detail out.
    LayerData layer{};
    const MaterialLayers* layers = material.layers;
    const bool detail = layers && layers->detail && materialDetail.load(std::memory_order_relaxed);
    if (layers && (detail || layers->layer)) {
        auto slot = [&](TextureHandle t) { return t.id ? textureSlot(t.id) : 0u; };
        auto binding = [&](MaterialSlot s) -> const TextureBinding& {
            return layers->bindings[size_t(s) - kMaterialSlots];
        };
        if (detail) {
            layer.flags |= kLayerDetail;
            layer.detailColor = textureSlot(layers->detailColorTexture.id);
            layer.detailNormal = slot(layers->detailNormalTexture);
            layer.detailMask = textureSlot(layers->detailMaskTexture.id);
            layer.detailMaskChannel = std::min(layers->detailMaskChannel, 3u);
            layer.detailStrength = layers->detailStrength;
            layer.detailNormalScale = layers->detailNormalScale;
            layer.detailRoughness = layers->detailRoughness;
            layer.detailRoughnessStrength = layers->detailRoughnessStrength;
            layer.detailFade = layers->detailFade;
            if (layer.detailNormal && !vertexTangents(binding(MaterialSlot::DetailNormal)))
                layer.flags |= kLayerDetailDerivativeTangents;
        }
        if (layers->layer) {
            layer.flags |= kLayerSecond;
            layer.layerMask = textureSlot(layers->layerMaskTexture.id);
            layer.layerMaskSource = std::min(layers->layerMask, 6u);
            layer.layerBaseColor = textureSlot(layers->layerBaseColorTexture.id);
            layer.layerNormal = slot(layers->layerNormalTexture);
            layer.layerMetallicRoughness = textureSlot(layers->layerMetallicRoughnessTexture.id);
            layer.layerBaseColorFactor = layers->layerBaseColor;
            layer.layerMetallic = layers->layerMetallic;
            layer.layerRoughness = layers->layerRoughness;
            layer.layerNormalScale = layers->layerNormalScale;
            if (layer.layerNormal && !vertexTangents(binding(MaterialSlot::LayerNormal)))
                layer.flags |= kLayerSecondDerivativeTangents;
        }
        for (size_t i = 0; i < kLayerSlots; ++i) {
            const TextureBinding& b = layers->bindings[i];
            SlotBinding& out = layer.slots[i];
            out.offset = b.transform.offset;
            out.scale = b.transform.scale;
            out.rotation = b.transform.rotation;
            out.samplerAndSet = samplerSlot(b.sampler) | (b.uvSet == 1 ? kSlotUvSet1 : 0u);
        }
    }
    // Baked lighting (G05, G06): a lightmap needs the mesh's lightmap UVs; without them, or
    // with the environment asked for, the draw is exactly what it was before.
    command.data.lightmapUvBuffer = 0;
    command.data.lightmapTexture = 0;
    command.data.indirectSource = kIndirectEnvironment;
    command.data.probes = 0;
    ProbeData probe{};
    if (const IndirectLighting* indirect = material.indirect) {
        if (indirect->source == IndirectLighting::Source::Lightmap && mesh.lightmapUvAddress && indirect->lightmap.id) {
            command.data.indirectSource = kIndirectLightmap;
            command.data.lightmapUvBuffer = mesh.lightmapUvAddress;
            command.data.lightmapTexture = textureSlot(indirect->lightmap.id);
        } else if (indirect->source == IndirectLighting::Source::Probes) {
            command.data.indirectSource = kIndirectProbes;
            for (size_t i = 0; i < 9; ++i) probe.sh[i] = glm::vec4(indirect->sh[i], 0.0f);
        }
    }
    command.data.alphaCutoff = material.alphaCutoff;
    command.doubleSided = material.doubleSided;
    command.blend = material.blend;
    command.mirrored = determinant < 0;
    const auto center = viewProjection * transform * glm::vec4(0, 0, 0, 1);
    command.depth = center.w != 0 ? center.z / center.w : 0;
    command.indexBuffer = mesh.indices.buffer;
    command.indexCount = mesh.indexCount;
    command.castShadows = material.castShadows && !material.blend;
    command.key = key;
    const float scale = std::sqrt(std::max(
        {glm::dot(glm::vec3(transform[0]), glm::vec3(transform[0])),
         glm::dot(glm::vec3(transform[1]), glm::vec3(transform[1])),
         glm::dot(glm::vec3(transform[2]), glm::vec3(transform[2]))}));
    if (!mesh.skinAddress) joints = {};
    const glm::vec4 bounds = joints.empty() ? mesh.bounds : skinnedBounds(mesh, joints);
    command.bounds = glm::vec4(glm::vec3(transform * glm::vec4(glm::vec3(bounds), 1.0f)), bounds.w * scale);

    if (mesh.lodCount > 0) pickLod(mesh, scale, command.bounds, command);   // levels of detail
    command.visible = viewFrustum.intersects(command.bounds);
    // Local reflection probes (G07): up to two whose boxes these bounds touch.
    command.data.reflections = pickReflections(command.bounds);
    command.data.decalLayers = material.blend ? 0u : material.decalLayers & kDecalLayerMask;   // G08
    // G09: filled in by prepareHistory when the draw has a previous pose.
    command.data.previousMvp = glm::mat4(1.0f);
    command.data.previousVertexBuffer = 0;
    command.data.history = 0;
    command.data.pad = 0;

    const uint32_t thread = JobSystem::threadIndex();
    auto add = [&](DrawList& list, uint32_t index) {
        command.skinList = index;   // the list it's in, for its layers too
        if (layer.flags) {
            command.layers = static_cast<uint32_t>(list.layers.size());
            list.layers.push_back(layer);
        }
        if (command.data.indirectSource == kIndirectProbes) {
            command.probes = static_cast<uint32_t>(list.probes.size());
            list.probes.push_back(probe);
        }
        addDraw(list, index, command, handle.id, joints);
    };
    if (thread < drawLists.size()) {
        add(drawLists[thread], thread);
    } else {
        std::lock_guard lock(foreignDrawMutex);
        add(foreignDraws, static_cast<uint32_t>(drawLists.size()));
    }
}

void VulkanRenderer::Impl::clearDrawLists() {
    for (DrawList& list : drawLists) {
        list.commands.clear();
        list.skins.clear();
        list.joints.clear();
        list.layers.clear();
        list.probes.clear();
    }
    std::lock_guard lock(foreignDrawMutex);
    foreignDraws.commands.clear();
    foreignDraws.skins.clear();
    foreignDraws.joints.clear();
    foreignDraws.layers.clear();
    foreignDraws.probes.clear();
    skinDispatches.clear();
}

void VulkanRenderer::Impl::recreateSwapchain() {
    vkDeviceWaitIdle(context->device());
    collectRetired(true);
    destroyRenderFinishedSemaphores();
    destroyBloomTargets();
    destroyAaTarget(aaTarget);
    destroyDecalTargets(decalTargets);
    destroyAoTargets(aoTargets);
    destroyTemporalTargets(temporalTargets);
    destroyParticleBackground(particleBackground);
    destroyImage(sceneColor);
    destroyImage(sceneDepth);
    destroyDisplayUiView();
    destroyImage(displayColor);
    swapchain->recreate(window.pixelWidth(), window.pixelHeight());
    createRenderTargets();
    createRenderFinishedSemaphores();
    needsResize = false;
    needsSceneResize = false;
    logSettingFallbacks();   // the present mode may have changed
}

bool VulkanRenderer::Impl::beginFrame() {
    clearDrawLists();
    clearParticles();
    uiCommands.clear();
    {
        std::lock_guard lock(lightMutex);
        lights.clear();
    }
    {
        std::lock_guard lock(decalMutex);
        submittedDecals.clear();
    }
    if (window.isMinimized()) return false;
    if (needsResize) {
        recreateSwapchain();
    } else if (needsSceneResize) {
        recreateSceneTargets();
    }

    VkDevice device = context->device();
    Frame& f = frames[frameIndex];

    // Wait until the GPU has finished the frame that last used this slot.
    if (frameNumber >= kFramesInFlight) {
        const uint64_t waitValue = frameNumber + 1 - kFramesInFlight;
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &frameTimeline;
        waitInfo.pValues = &waitValue;
        SW_PROFILE_SCOPE("Wait for GPU (frames in flight)");
        vk::check(vkWaitSemaphores(device, &waitInfo, UINT64_MAX), "vkWaitSemaphores");
    }
    readTimestamps(f);   // the GPU is done with this slot's last frame
    applyTextureSwaps();
    rebuildSamplers();
    // Settings that change images: the new ones made, the old ones retired, no GPU wait.
    collectRetired(false);
    if (needsShadowRebuild && rebuildShadowMaps()) needsShadowRebuild = false;
    updateBloomTargets();
    updateAaTarget();
    updateAoTargets();
    updateDecalTargets();
    updateTemporalTargets();

    SW_PROFILE_SCOPE("Acquire swapchain image");
    VkResult acquire =
        vkAcquireNextImageKHR(device, swapchain->handle(), UINT64_MAX, f.imageAcquired, VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        needsResize = true;
        return false;
    }
    if (acquire != VK_SUBOPTIMAL_KHR) vk::check(acquire, "vkAcquireNextImageKHR");
    return true;
}

void VulkanRenderer::Impl::readTimestamps(Frame& f) {
    if (f.timedPasses.empty()) return;
    std::array<uint64_t, vk::RenderGraph::kMaxTimedPasses + 1> ticks{};
    const uint32_t count = static_cast<uint32_t>(f.timedPasses.size()) + 1;
    const VkResult result = vkGetQueryPoolResults(
        context->device(), f.timestamps, 0, count, sizeof(uint64_t) * count, ticks.data(), sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS) return;
    const double msPerTick = static_cast<double>(context->timestampPeriod()) * 1e-6;
    passTimings.resize(f.timedPasses.size());
    for (size_t i = 0; i < f.timedPasses.size(); ++i) {
        passTimings[i].name = f.timedPasses[i];
        passTimings[i].gpuMs = static_cast<float>(static_cast<double>(ticks[i + 1] - ticks[i]) * msPerTick);
    }
    gpuFrameMs = static_cast<float>(static_cast<double>(ticks[count - 1] - ticks[0]) * msPerTick);
    f.timedPasses.clear();
}

float VulkanRenderer::Impl::waitForPresent() {
    if (frameNumber == 0) return 0.0f;
    SW_PROFILE_SCOPE("Wait for present (low latency)");
    VkDevice device = context->device();
    const uint64_t timeoutNs = 100'000'000;   // never stall a frame on a lost present
    bool presented = false;
    if (presentedSwapchain == swapchain->handle() && !needsResize) {
        if (context->presentWait() == vk::Context::PresentWait::Version2) {
            VkPresentWait2InfoKHR info{};
            info.sType = VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR;
            info.presentId = frameNumber;
            info.timeout = timeoutNs;
            presented = vkWaitForPresent2KHR(device, presentedSwapchain, &info) == VK_SUCCESS;
        } else if (context->presentWait() == vk::Context::PresentWait::Version1) {
            presented = vkWaitForPresentKHR(device, presentedSwapchain, frameNumber, timeoutNs) == VK_SUCCESS;
        }
    }
    if (context->presentWait() == vk::Context::PresentWait::None) {
        // No present wait: the next best thing is the GPU having finished the frame.
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &frameTimeline;
        waitInfo.pValues = &frameNumber;
        vkWaitSemaphores(device, &waitInfo, timeoutNs);
        return 0.0f;
    }
    if (!presented) {
        lastPresentTicks = 0;
        return 0.0f;
    }
    const uint64_t now = SDL_GetPerformanceCounter();
    const float interval = lastPresentTicks ? static_cast<float>(
                                                  static_cast<double>(now - lastPresentTicks) * 1000.0 /
                                                  static_cast<double>(SDL_GetPerformanceFrequency()))
                                            : 0.0f;
    lastPresentTicks = now;
    return interval;
}

void VulkanRenderer::Impl::setSceneViewport(VkCommandBuffer cmd) {
    const VkExtent2D extent = sceneColor.extent;
    // Negative height flips Y so standard Y-up projection matrices work unchanged.
    VkViewport viewport{0.0f,
                        static_cast<float>(extent.height),
                        static_cast<float>(extent.width),
                        -static_cast<float>(extent.height),
                        0.0f,
                        1.0f};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void VulkanRenderer::Impl::recordGrid(VkCommandBuffer cmd) {
    if (!grid.enabled) return;
    GridPushConstants push;
    push.viewProjection = viewProjection;
    push.cameraPosition = glm::vec4(cameraPosition, std::max(grid.fadeDistance, 1.0f));
    push.params = glm::vec4(
        std::max(grid.cellSize, 1e-3f), static_cast<float>(std::max(grid.majorEvery, 1)),
        std::clamp(grid.opacity, 0.0f, 1.0f), 0.0f);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gridPipeline);
    vkCmdPushConstants(
        cmd, gridLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

void VulkanRenderer::Impl::prepareDraws(Frame& f) {
    auto& draws = gatheredDraws;
    draws.clear();
    lastCulledDraws = 0;
    lastUnkeyedDraws = 0;
    uint32_t total = 0;
    for (const DrawList& list : drawLists) total += static_cast<uint32_t>(list.commands.size());
    {
        std::lock_guard lock(foreignDrawMutex);
        total += static_cast<uint32_t>(foreignDraws.commands.size());
        lastDrawCount = total;
        lastLodStats = {};
        if (total == 0) return;
        draws.reserve(total);
        for (const auto& list : drawLists) draws.insert(draws.end(), list.commands.begin(), list.commands.end());
        draws.insert(draws.end(), foreignDraws.commands.begin(), foreignDraws.commands.end());
    }
    ensureDrawCapacity(f, total);
    // What levels of detail saved.
    for (const DrawCommand& draw : draws) {
        lastCulledDraws += !draw.visible;
        lastUnkeyedDraws += !draw.key.valid();
        lastLodStats.triangles += draw.indexCount / 3;
        lastLodStats.fullTriangles += (draw.fullIndexCount ? draw.fullIndexCount : draw.indexCount) / 3;
        lastLodStats.reducedDraws += draw.fullIndexCount != 0;
    }
    const auto transparent =
        std::stable_partition(draws.begin(), draws.end(), [](const auto& draw) { return !draw.blend; });
    // Opaque draws by pipeline, then nearest first, so nearer surfaces fill the depth buffer
    // first and hidden ones fail the early depth test instead of being shaded (reverse-Z:
    // greater depth is nearer).
    std::sort(draws.begin(), transparent, [this](const auto& a, const auto& b) {
        const VkPipeline pa = meshPipelineFor(a), pb = meshPipelineFor(b);
        return pa != pb ? pa < pb : a.depth > b.depth;
    });
    // Reverse-Z is smaller farther away. Object-level sorting is the basic
    // transparency path; intersecting transparent surfaces need a later solution.
    std::stable_sort(transparent, draws.end(), [](const auto& a, const auto& b) { return a.depth < b.depth; });
    prepareHistory();   // previous poses first: skinning skins them too (G09)
    prepareSkinning(f);
    // One draw table serves the scene and every shadow map; draws with detail or second
    // layers point at their entries in the layer table.
    uint32_t layerCount = 0;
    for (const DrawCommand& draw : draws) layerCount += draw.layers != kNoLayers;
    if (layerCount) ensureLayerCapacity(f, layerCount);
    // Draws lit by probes point at their samples the same way (G06).
    uint32_t probeCount = 0;
    for (const DrawCommand& draw : draws) probeCount += draw.probes != kNoLayers;
    if (probeCount) ensureProbeCapacity(f, probeCount);
    uint32_t nextLayer = 0, nextProbe = 0;
    std::lock_guard lock(foreignDrawMutex);
    for (uint32_t i = 0; i < draws.size(); ++i) {
        draws[i].drawIndex = i;
        f.drawData[i] = draws[i].data;
        const DrawList& list = draws[i].skinList < drawLists.size() ? drawLists[draws[i].skinList] : foreignDraws;
        if (draws[i].probes != kNoLayers) {
            f.probeData[nextProbe] = list.probes[draws[i].probes];
            f.drawData[i].probes = f.probeAddress + VkDeviceAddress(nextProbe) * sizeof(ProbeData);
            ++nextProbe;
        }
        if (draws[i].layers == kNoLayers) continue;
        f.layerData[nextLayer] = list.layers[draws[i].layers];
        f.drawData[i].layers = f.layerAddress + VkDeviceAddress(nextLayer) * sizeof(LayerData);
        ++nextLayer;
    }
    vk::check(
        vmaFlushAllocation(context->allocator(), f.drawBuffer.allocation, 0, sizeof(DrawData) * draws.size()),
        "vmaFlushAllocation");
    if (layerCount)
        vk::check(
            vmaFlushAllocation(context->allocator(), f.layerBuffer.allocation, 0, sizeof(LayerData) * layerCount),
            "vmaFlushAllocation");
    if (probeCount)
        vk::check(
            vmaFlushAllocation(context->allocator(), f.probeBuffer.allocation, 0, sizeof(ProbeData) * probeCount),
            "vmaFlushAllocation");
}

VkPipeline VulkanRenderer::Impl::meshPipelineFor(const DrawCommand& draw) const {
    if (!draw.blend && draw.data.alphaCutoff < 0.0f)
        return solidPipelines[(draw.mirrored ? 2 : 0) + (draw.doubleSided ? 1 : 0)];
    return draw.mirrored ? mirroredPipelines[(draw.blend ? 2 : 0) + (draw.doubleSided ? 1 : 0)]
           : draw.blend  ? (draw.doubleSided ? meshBlendDoubleSidedPipeline : meshBlendPipeline)
                         : (draw.doubleSided ? meshDoubleSidedPipeline : meshPipeline);
}

void VulkanRenderer::Impl::recordDraws(VkCommandBuffer cmd, Frame& f) {
    if (gatheredDraws.empty()) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &bindlessSet, 0, nullptr);
    DrawPushConstants push{f.drawAddress, f.frameAddress};
    vkCmdPushConstants(
        cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

    VkBuffer boundIndices = VK_NULL_HANDLE;
    VkPipeline boundPipeline = meshPipeline;
    for (const auto& draw : gatheredDraws) {
        if (!draw.visible) continue;
        // Over the surface prepass, opaque surfaces shade where its depth is EQUAL to theirs.
        const VkPipeline pipeline =
            prepassActive() && !draw.blend ? depthEqualPipelines[prepassIndex(draw)] : meshPipelineFor(draw);
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

void VulkanRenderer::Impl::endFrame() {
    Frame& f = frames[frameIndex];
    VkImage image = swapchain->image(imageIndex);
    VkCommandBuffer cmd = f.cmd;

    vk::check(vkResetCommandPool(context->device(), f.pool, 0), "vkResetCommandPool");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk::check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");

    decideHistory();
    prepareDraws(f);
    prepareParticles();
    prepareDecals();
    writeFrameData(f);
    buildGraph(cmd, f, image);
    graph.setTimestampPool(f.timestamps);
    graph.execute(cmd);
    f.timedPasses = graph.timedPasses();
    frameGraphText = "view " + std::to_string(currentView.value) + (currentView.valid() ? "" : " (none: no history)") +
                     ", " + std::to_string(lastUnkeyedDraws) + " of " + std::to_string(lastDrawCount) +
                     " draws without an instance key\n" + graph.description();
    // Logged when it starts and stops, not every frame.
    if ((graph.untimedPasses() > 0) != (untimedPasses > 0)) {
        if (graph.untimedPasses())
            SW_LOG(
                LogRender, Warning, "{} render passes are past the GPU timer's {} and go untimed",
                graph.untimedPasses(), vk::RenderGraph::kMaxTimedPasses);
        else
            SW_LOG(LogRender, Info, "every render pass is timed again");
    }
    untimedPasses = graph.untimedPasses();

    vk::check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    ++frameNumber;
    if (frameNumber == 1) logSettingFallbacks();
    VkSemaphore finished = renderFinished[imageIndex];

    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = f.imageAcquired;
    // First use of the swapchain image: the blit, or the GUI pass in editor mode. Must
    // match the stages the swapchain image is imported with in buildGraph.
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signalInfos[2]{};
    signalInfos[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfos[0].semaphore = finished;
    signalInfos[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signalInfos[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfos[1].semaphore = frameTimeline;
    signalInfos[1].value = frameNumber;
    signalInfos[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = cmd;

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &waitInfo;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = 2;
    submit.pSignalSemaphoreInfos = signalInfos;

    VkSwapchainKHR sc = swapchain->handle();
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &finished;
    present.swapchainCount = 1;
    present.pSwapchains = &sc;
    present.pImageIndices = &imageIndex;
    // Present id N for frame N, so present wait can tell when it reached the screen.
    const uint64_t presentId = frameNumber;
    VkPresentIdKHR presentId1{VK_STRUCTURE_TYPE_PRESENT_ID_KHR, nullptr, 1, &presentId};
    VkPresentId2KHR presentId2{VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR, nullptr, 1, &presentId};
    if (context->presentWait() == vk::Context::PresentWait::Version2) present.pNext = &presentId2;
    if (context->presentWait() == vk::Context::PresentWait::Version1) present.pNext = &presentId1;

    VkResult result;
    {
        std::lock_guard lock(queueMutex);
        vk::check(vkQueueSubmit2(context->graphicsQueue(), 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2");
        result = vkQueuePresentKHR(context->graphicsQueue(), &present);
    }
    presentedSwapchain = sc;
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        needsResize = true;
    } else {
        vk::check(result, "vkQueuePresentKHR");
    }

    finishHistory();
    if (screenshotRequested) finishScreenshot();
    if (captureRequested) finishCapture();
    frameIndex = (frameIndex + 1) % kFramesInFlight;
}

void VulkanRenderer::Impl::writeFrameData(Frame& f) {
    FrameData header{};
    header.cameraPosition = cameraPosition;
    header.skyAmbient = lighting.skyAmbient;
    header.groundAmbient = lighting.groundAmbient;
    header.debugView = static_cast<uint32_t>(debugView);
    header.brdfTable = brdfSlot;
    header.specularLevels = static_cast<float>(specularCube.levels);
    // The prepass may run for decals alone (G08): occlusion only when it's on.
    header.aoTexture = prepassActive() && effectiveAmbientOcclusion() != AmbientOcclusion::Off ? aoTargets.slot : 0u;
    header.view = view;
    header.projection = projection;
    header.inverseProjection = glm::inverse(projection);
    header.renderSize = glm::vec2(sceneColor.extent.width, sceneColor.extent.height);
    header.aoRadius = std::isfinite(lighting.aoRadius) ? std::clamp(lighting.aoRadius, 0.01f, 10.0f) : 0.3f;
    header.aoIntensity = std::isfinite(lighting.aoIntensity) ? std::clamp(lighting.aoIntensity, 0.0f, 4.0f) : 1.0f;
    // A capture frame is always the lit view: a probe is never built from a debug view.
    if (captureRequested) header.debugView = static_cast<uint32_t>(DebugView::Lit);
    header.reflectionProbes = f.frameAddress + kReflectionProbesOffset;
    header.reflectionProbeCount = writeReflectionProbes(f);
    writeDecals(f);
    const bool decals = decalsActive() && !frameDecals.empty();
    header.decalAlbedo = decals ? decalSlots[decalTargets.pair][0] : 0u;
    header.decalMaterial = decals ? decalSlots[decalTargets.pair][1] : 0u;
    header.jitter = jittered ? jitter : glm::vec2(0.0f);
    auto* out = reinterpret_cast<LightData*>(f.frameData + sizeof(FrameData));
    auto* views = reinterpret_cast<ShadowView*>(f.frameData + kShadowViewsOffset);
    std::vector<LightDesc> used;
    {
        std::lock_guard lock(lightMutex);
        lastDroppedLights = lights.size() > kMaxLights ? static_cast<uint32_t>(lights.size()) - kMaxLights : 0;
        used.assign(lights.begin(), lights.begin() + std::min<size_t>(lights.size(), kMaxLights));
    }
    lastLightCount = static_cast<uint32_t>(used.size());
    if (used.empty()) {
        // No lights in the scene: a soft default sun, so content is never pitch black.
        LightDesc sun;
        sun.type = LightDesc::Type::Directional;
        sun.direction = -glm::normalize(glm::vec3(0.4f, 0.8f, 0.3f));
        sun.color = glm::vec3(1.0f, 0.96f, 0.9f) * 2.5f;
        sun.range = 1.0f;
        sun.castShadows = true;
        used.push_back(sun);
    }
    // Directional lights first, every pixel's short list; then point and spot lights, which
    // clusters pick from (G11). Each keeps its order, so the shadows they get don't change.
    const auto locals = std::stable_partition(
        used.begin(), used.end(), [](const LightDesc& l) { return l.type == LightDesc::Type::Directional; });
    header.directionalCount = static_cast<uint32_t>(locals - used.begin());
    const uint32_t count = static_cast<uint32_t>(used.size());
    for (uint32_t i = 0; i < count; ++i) {
        const LightDesc& light = used[i];
        LightData& d = out[i];
        d = {};
        d.position = light.position;
        d.type = static_cast<uint32_t>(light.type);
        const float length = glm::length(light.direction);
        d.direction = length > 1e-6f ? light.direction / length : glm::vec3(0, 0, -1);
        d.range = std::max(light.range, 0.01f);
        d.color = light.color;
        d.cosOuter = light.cosOuter;
        d.cosInner = std::max(light.cosInner, light.cosOuter + 1e-4f);
        d.shadowView = kNoShadow;
    }
    planShadows(used, out, count, views, header);
    header.lightCount = count;
    prepareClusters(f, header, count - header.directionalCount);
    header.shadowViews = f.frameAddress + kShadowViewsOffset;
    std::memcpy(f.frameData, &header, sizeof(header));
    vk::check(
        vmaFlushAllocation(context->allocator(), f.frameBuffer.allocation, 0, VK_WHOLE_SIZE), "vmaFlushAllocation");
}

void VulkanRenderer::Impl::buildGraph(VkCommandBuffer, Frame& f, VkImage swapchainImage) {
    graph.reset();
    vk::RenderGraph::ImageId motionImage = kNoGraphImage;
    using vk::ImageUse;
    const auto color =
        graph.import("scene color (HDR)", sceneColor.image, VK_IMAGE_ASPECT_COLOR_BIT, sceneColorState, true);
    const auto depth = graph.import("scene depth", sceneDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT, sceneDepthState, true);
    const auto display = graph.import("display", displayColor.image, VK_IMAGE_ASPECT_COLOR_BIT, displayState, true);
    // Freshly acquired: the acquire semaphore's wait stages stand in for "last use".
    swapchainState = {
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE};
    const auto backbuffer = graph.import("swapchain", swapchainImage, VK_IMAGE_ASPECT_COLOR_BIT, swapchainState, true);

    // The lighting cubes keep their own layouts and barriers: only this pass writes them,
    // and only when the environment changed.
    if (environmentChanged())
        graph.addPass("environment lighting", {}, [this](VkCommandBuffer cmd) { recordEnvironment(cmd); });
    // Skinned vertices: written by compute, then pulled by the shadow and scene vertex shaders.
    std::vector<vk::RenderGraph::BufferAccess> vertexReads;
    if (!skinDispatches.empty()) {
        const auto skinned = graph.importBuffer("skinned vertices", f.skinnedBuffer.buffer, f.skinnedState);
        graph.addPass(
            "skinning", {}, [this](VkCommandBuffer cmd) { recordSkinning(cmd); },
            {{skinned, vk::BufferUse::ComputeWrite}});
        vertexReads.push_back({skinned, vk::BufferUse::VertexShaderRead});
    }
    std::vector<vk::RenderGraph::Use> sceneUses = {
        {color, ImageUse::ColorAttachment}, {depth, ImageUse::DepthAttachment}};
    if (!cascadePasses.empty()) {
        const auto atlas =
            graph.import("sun shadows", cascadeAtlas.image, VK_IMAGE_ASPECT_DEPTH_BIT, cascadeState, true);
        graph.addPass(
            "shadows (sun)", {{atlas, ImageUse::DepthAttachment}},
            [this, &f](VkCommandBuffer cmd) { recordShadows(cmd, f, cascadePasses, cascadeAtlas); }, vertexReads);
        sceneUses.push_back({atlas, ImageUse::Sampled});
    }
    if (!localPasses.empty()) {
        const auto atlas = graph.import("light shadows", localAtlas.image, VK_IMAGE_ASPECT_DEPTH_BIT, localState, true);
        graph.addPass(
            "shadows (lights)", {{atlas, ImageUse::DepthAttachment}},
            [this, &f](VkCommandBuffer cmd) { recordShadows(cmd, f, localPasses, localAtlas); }, vertexReads);
        sceneUses.push_back({atlas, ImageUse::Sampled});
    }
    if (prepassActive()) {
        // Surface prepass, then ambient occlusion from it; the scene tests its depth without writing.
        const auto normals =
            graph.import("surface normals", aoTargets.normal.image, VK_IMAGE_ASPECT_COLOR_BIT, aoNormalState, true);
        std::vector<vk::RenderGraph::Use> prepassUses = {
            {depth, ImageUse::DepthAttachment}, {normals, ImageUse::ColorAttachment}};
        if (temporalActive()) {
            // Motion vectors (G09): written every frame the prepass runs, every pixel cleared.
            const auto motion = graph.import(
                "motion vectors", temporalTargets.motion.image, VK_IMAGE_ASPECT_COLOR_BIT, motionState, true);
            prepassUses.push_back({motion, ImageUse::ColorAttachment});
            motionImage = motion;
        }
        if (decalsActive()) {
            const auto layers = graph.import(
                "decal layers", decalTargets.layers.image, VK_IMAGE_ASPECT_COLOR_BIT, decalLayerState, true);
            prepassUses.push_back({layers, ImageUse::ColorAttachment});
            graph.addPass(
                "surface prepass", prepassUses, [this, &f](VkCommandBuffer cmd) { recordPrepass(cmd, f); },
                vertexReads);
            if (!frameDecals.empty()) {
                // Decals (G08): into their two buffers, which the scene's opaque shading reads.
                const auto albedo = graph.import(
                    "decal colour", decalTargets.albedo.image, VK_IMAGE_ASPECT_COLOR_BIT, decalAlbedoState, true);
                const auto material = graph.import(
                    "decal roughness and metallic", decalTargets.material.image, VK_IMAGE_ASPECT_COLOR_BIT,
                    decalMaterialState, true);
                graph.addPass(
                    "decals",
                    {{depth, ImageUse::DepthSampled},
                     {normals, ImageUse::Sampled},
                     {layers, ImageUse::Sampled},
                     {albedo, ImageUse::ColorAttachment},
                     {material, ImageUse::ColorAttachment}},
                    [this, &f](VkCommandBuffer cmd) { recordDecals(cmd, f); });
                sceneUses.push_back({albedo, ImageUse::Sampled});
                sceneUses.push_back({material, ImageUse::Sampled});
            }
        } else {
            graph.addPass(
                "surface prepass", prepassUses, [this, &f](VkCommandBuffer cmd) { recordPrepass(cmd, f); },
                vertexReads);
        }
        // The occlusion images keep their own barriers, like bloom's: only this pass writes
        // them and only the scene reads them.
        if (effectiveAmbientOcclusion() != AmbientOcclusion::Off)
            graph.addPass(
                "ambient occlusion", {{depth, ImageUse::ComputeSampled}, {normals, ImageUse::ComputeSampled}},
                [this, &f](VkCommandBuffer cmd) { recordAmbientOcclusion(cmd, f); });
        sceneUses[1] = {depth, ImageUse::DepthReadOnly};
    }
    std::vector<vk::RenderGraph::BufferAccess> sceneReads = vertexReads;
    std::optional<vk::RenderGraph::BufferId> clusterLists;
    if (clustersThisFrame) {
        // G11: each cluster's lights, then the scene's fragments read them (and particles, A04).
        const auto lists = graph.importBuffer("light clusters", f.clusterBuffer.buffer, f.clusterState);
        clusterLists = lists;
        graph.addPass(
            "light clusters", {}, [this, &f](VkCommandBuffer cmd) { recordClusters(cmd, f); },
            {{lists, vk::BufferUse::ComputeWrite}});
        sceneReads.push_back({lists, vk::BufferUse::FragmentShaderRead});
    }
    graph.addPass("scene", sceneUses, [this, &f](VkCommandBuffer cmd) { recordScene(cmd, f); }, sceneReads);
    if (captureRequested) {
        // Reflection capture (G07): the HDR scene as drawn, before particles, exposure and the rest.
        graph.addPass("reflection capture readback", {{color, ImageUse::TransferSrc}}, [this](VkCommandBuffer cmd) {
            recordCaptureCopy(cmd);
        });
    }
    if (!particleDraws.empty()) {
        // A04: opaque depth-writing batches first when the main pass keeps depth read-only
        // (soft particles, distortion); distortion's copy of the scene; then the particles.
        particleFrameAddress = f.frameAddress;
        if (particleDepthPassThisFrame)
            graph.addPass(
                "particles (depth writing)", {{color, ImageUse::ColorAttachment}, {depth, ImageUse::DepthAttachment}},
                [this](VkCommandBuffer cmd) { recordParticles(cmd, true); });
        std::vector<vk::RenderGraph::Use> particleUses = {{color, ImageUse::ColorAttachment}};
        if (particleBackgroundThisFrame) {
            const auto background = graph.import(
                "particle background", particleBackground.image.image, VK_IMAGE_ASPECT_COLOR_BIT,
                particleBackgroundState, true);
            graph.addPass(
                "particle background copy", {{color, ImageUse::TransferSrc}, {background, ImageUse::TransferDst}},
                [this](VkCommandBuffer cmd) { recordParticleBackgroundCopy(cmd); });
            particleUses.push_back({background, ImageUse::Sampled});
        }
        if (particleSoftThisFrame) {
            particleUses.push_back({depth, ImageUse::DepthReadOnly});
            particleUses.push_back({depth, ImageUse::DepthSampled});
        } else {
            particleUses.push_back({depth, ImageUse::DepthAttachment});
        }
        std::vector<vk::RenderGraph::BufferAccess> particleReads;
        if (clusterLists) particleReads.push_back({*clusterLists, vk::BufferUse::FragmentShaderRead});
        graph.addPass(
            "particles", particleUses, [this](VkCommandBuffer cmd) { recordParticles(cmd, false); }, particleReads);
    }
    if (temporalThisFrame && motionImage != kNoGraphImage) {
        // G09, G10: the reactive mask, then the resolve into this frame's history, copied back.
        const auto reactive = graph.import(
            "reactive mask", temporalTargets.reactive.image, VK_IMAGE_ASPECT_COLOR_BIT, reactiveState, true);
        graph.addPass(
            "reactive mask", {{reactive, ImageUse::ColorAttachment}, {depth, ImageUse::DepthReadOnly}},
            [this, &f](VkCommandBuffer cmd) { recordReactive(cmd, f); });
        const uint32_t w = historyWrite, r = historyWrite ^ 1u;
        const auto historyIn = graph.import(
            "history (last frame)", temporalTargets.color[r].image, VK_IMAGE_ASPECT_COLOR_BIT, historyColorState[r],
            false);
        const auto distanceIn = graph.import(
            "history distance (last frame)", temporalTargets.distance[r].image, VK_IMAGE_ASPECT_COLOR_BIT,
            historyDistanceState[r], false);
        const auto historyOut = graph.import(
            "history", temporalTargets.color[w].image, VK_IMAGE_ASPECT_COLOR_BIT, historyColorState[w], true);
        const auto distanceOut = graph.import(
            "history distance", temporalTargets.distance[w].image, VK_IMAGE_ASPECT_COLOR_BIT, historyDistanceState[w],
            true);
        graph.addPass(
            "taa",
            {{color, ImageUse::ComputeSampled},
             {depth, ImageUse::ComputeSampled},
             {motionImage, ImageUse::ComputeSampled},
             {reactive, ImageUse::ComputeSampled},
             {historyIn, ImageUse::ComputeSampled},
             {distanceIn, ImageUse::ComputeSampled},
             {historyOut, ImageUse::StorageWrite},
             {distanceOut, ImageUse::StorageWrite}},
            [this, &f](VkCommandBuffer cmd) { recordTaa(cmd, f); });
        graph.addPass(
            "taa resolve copy", {{historyOut, ImageUse::TransferSrc}, {color, ImageUse::TransferDst}},
            [this](VkCommandBuffer cmd) { recordTaaCopy(cmd); });
    }
    // The bloom chain keeps its own barriers, like the lighting cubes: only this pass
    // writes it and only the tonemap reads it.
    if (bloomIntensity() > 0.0f)
        graph.addPass("bloom", {{color, ImageUse::ComputeSampled}}, [this](VkCommandBuffer cmd) { recordBloom(cmd); });
    if (fxaaActive()) {
        // Tone mapping into FXAA's input, then FXAA into the display image, before game UI.
        const auto aaInput =
            graph.import("anti-aliasing input", aaTarget.image.image, VK_IMAGE_ASPECT_COLOR_BIT, aaState, true);
        graph.addPass(
            "tonemap", {{color, ImageUse::Sampled}, {aaInput, ImageUse::ColorAttachment}},
            [this](VkCommandBuffer cmd) { recordTonemap(cmd); });
        graph.addPass(
            "fxaa", {{aaInput, ImageUse::Sampled}, {display, ImageUse::ColorAttachment}},
            [this](VkCommandBuffer cmd) { recordFxaa(cmd); });
    } else {
        graph.addPass(
            "tonemap", {{color, ImageUse::Sampled}, {display, ImageUse::ColorAttachment}},
            [this](VkCommandBuffer cmd) { recordTonemap(cmd); });
    }
    if (!uiCommands.empty()) {
        graph.addPass(
            "ui", {{display, ImageUse::ColorAttachment}}, [this, &f](VkCommandBuffer cmd) { recordUi(cmd, f); });
    }
    if (screenshotRequested) {
        graph.addPass("screenshot readback", {{display, ImageUse::TransferSrc}}, [this](VkCommandBuffer cmd) {
            recordScreenshotCopy(cmd);
        });
    }
    if (viewportMode()) {
        // Editor: the GUI shows the display image inside its viewport panel.
        graph.addPass(
            "gui", {{display, ImageUse::Sampled}, {backbuffer, ImageUse::ColorAttachment}},
            [this](VkCommandBuffer cmd) { recordGui(cmd, swapchain->imageView(imageIndex), true); });
    } else {
        graph.addPass(
            "copy to window", {{display, ImageUse::TransferSrc}, {backbuffer, ImageUse::TransferDst}},
            [this, swapchainImage](VkCommandBuffer cmd) { recordCopyToWindow(cmd, swapchainImage); });
        if (guiEnabled) {
            graph.addPass("gui", {{backbuffer, ImageUse::ColorAttachment}}, [this](VkCommandBuffer cmd) {
                recordGui(cmd, swapchain->imageView(imageIndex), false);
            });
        }
    }
    graph.addPass("present", {{backbuffer, ImageUse::Present}}, nullptr);
}

void VulkanRenderer::Impl::recordScene(VkCommandBuffer cmd, Frame& f) {
    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = sceneColor.view;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = {{clearColor.r, clearColor.g, clearColor.b, clearColor.a}};
    // The occlusion view is white wherever nothing occludes, the background included.
    if (debugView == DebugView::AmbientOcclusion) colorAttachment.clearValue.color = {{1.0f, 1.0f, 1.0f, 1.0f}};

    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = sceneDepth.view;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    if (prepassActive()) {
        // The prepass's depth, tested and never written.
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    }
    // Kept when particles are drawn next, for their depth test, and for the reactive mask and TAA.
    depthAttachment.storeOp =
        particleDraws.empty() && !temporalThisFrame ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue.depthStencil = {0.0f, 0};   // reverse-Z: 0 is infinitely far

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, sceneColor.extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &colorAttachment;
    rendering.pDepthAttachment = &depthAttachment;
    vkCmdBeginRendering(cmd, &rendering);
    setSceneViewport(cmd);
    if (showsSky() && debugView != DebugView::AmbientOcclusion) recordSky(cmd);
    recordDraws(cmd, f);
    if (!captureRequested) recordGrid(cmd);   // the editor grid never goes into a reflection
    vkCmdEndRendering(cmd);
}

void VulkanRenderer::Impl::recordTonemap(VkCommandBuffer cmd) {
    VkRenderingAttachmentInfo target{};
    target.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    target.imageView = fxaaActive() ? aaTarget.image.view : displayColor.view;   // same size and format
    target.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    target.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // every pixel is written
    target.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, displayColor.extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &target;
    vkCmdBeginRendering(cmd, &rendering);
    const VkExtent2D extent = displayColor.extent;
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapLayout, 0, 1, &bindlessSet, 0, nullptr);
    TonemapPushConstants push{};
    push.outputSize = glm::vec2(extent.width, extent.height);
    push.sceneTexture = sceneSlot;
    push.exposureScale = std::exp2(std::clamp(lighting.exposure, -16.0f, 16.0f));
    // Debug views show raw values: no exposure or curve.
    push.tonemapper =
        debugView == DebugView::Lit || debugView == DebugView::Lighting || debugView == DebugView::ShadowCascades
            ? static_cast<uint32_t>(lighting.tonemapper)
            : 99u;
    push.bloomTexture = bloom.slot;
    push.bloomIntensity = bloomIntensity();
    vkCmdPushConstants(cmd, tonemapLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void VulkanRenderer::Impl::recordCopyToWindow(VkCommandBuffer cmd, VkImage swapchainImage) {
    // Same size; a blit rather than a copy because the formats differ (RGBA vs BGRA).
    const VkExtent2D out = swapchain->extent();
    VkImageBlit2 region{};
    region.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = {
        static_cast<int32_t>(displayColor.extent.width), static_cast<int32_t>(displayColor.extent.height), 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[1] = {static_cast<int32_t>(out.width), static_cast<int32_t>(out.height), 1};
    VkBlitImageInfo2 blit{};
    blit.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
    blit.srcImage = displayColor.image;
    blit.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    blit.dstImage = swapchainImage;
    blit.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    blit.regionCount = 1;
    blit.pRegions = &region;
    blit.filter = VK_FILTER_LINEAR;
    vkCmdBlitImage2(cmd, &blit);
}

}   // namespace shockwave
