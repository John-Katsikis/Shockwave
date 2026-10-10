#include "RendererImpl.h"

#include <Core/Profile.h>

namespace shockwave {

void Renderer::Impl::drawMesh(
    MeshHandle handle, const glm::mat4& transform, TextureHandle texture, const glm::vec4& tint,
    const DrawMaterial& material) {
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
    command.data.pad = 0;
    command.data.tint = tint;
    command.data.uvScale = material.uvScale;
    command.data.vertexBuffer = mesh.vertexAddress;
    command.data.textureIndex = texture.id;
    command.data.alphaCutoff = material.alphaCutoff;
    command.doubleSided = material.doubleSided;
    command.blend = material.blend;
    command.mirrored = determinant < 0;
    const auto center = viewProjection * transform * glm::vec4(0, 0, 0, 1);
    command.depth = center.w != 0 ? center.z / center.w : 0;
    command.indexBuffer = mesh.indices.buffer;
    command.indexCount = mesh.indexCount;
    command.castShadows = material.castShadows && !material.blend;
    const float scale = std::sqrt(std::max(
        {glm::dot(glm::vec3(transform[0]), glm::vec3(transform[0])),
         glm::dot(glm::vec3(transform[1]), glm::vec3(transform[1])),
         glm::dot(glm::vec3(transform[2]), glm::vec3(transform[2]))}));
    command.bounds = glm::vec4(glm::vec3(transform * glm::vec4(glm::vec3(mesh.bounds), 1.0f)), mesh.bounds.w * scale);

    const uint32_t thread = JobSystem::threadIndex();
    if (thread < drawLists.size()) {
        drawLists[thread].commands.push_back(command);
    } else {
        std::lock_guard lock(foreignDrawMutex);
        foreignDraws.commands.push_back(command);
    }
}

void Renderer::Impl::clearDrawLists() {
    for (DrawList& list : drawLists) list.commands.clear();
    std::lock_guard lock(foreignDrawMutex);
    foreignDraws.commands.clear();
}

void Renderer::Impl::recreateSwapchain() {
    vkDeviceWaitIdle(context->device());
    destroyRenderFinishedSemaphores();
    destroyImage(sceneColor);
    destroyImage(sceneDepth);
    destroyImage(displayColor);
    swapchain->recreate(window.pixelWidth(), window.pixelHeight());
    createRenderTargets();
    createRenderFinishedSemaphores();
    needsResize = false;
    needsSceneResize = false;
}

bool Renderer::Impl::beginFrame() {
    clearDrawLists();
    {
        std::lock_guard lock(lightMutex);
        lights.clear();
    }
    if (window.isMinimized()) return false;
    if (needsResize) {
        recreateSwapchain();
    } else if (needsSceneResize) {
        recreateSceneTargets();
    }
    if (needsShadowRebuild) {
        vkDeviceWaitIdle(context->device());
        destroyShadowMaps();
        createShadowMaps();
        needsShadowRebuild = false;
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

void Renderer::Impl::readTimestamps(Frame& f) {
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

float Renderer::Impl::waitForPresent() {
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

void Renderer::Impl::setSceneViewport(VkCommandBuffer cmd) {
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

void Renderer::Impl::recordGrid(VkCommandBuffer cmd) {
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

void Renderer::Impl::prepareDraws(Frame& f) {
    auto& draws = gatheredDraws;
    draws.clear();
    uint32_t total = 0;
    for (const DrawList& list : drawLists) total += static_cast<uint32_t>(list.commands.size());
    {
        std::lock_guard lock(foreignDrawMutex);
        total += static_cast<uint32_t>(foreignDraws.commands.size());
        lastDrawCount = total;
        if (total == 0) return;
        draws.reserve(total);
        for (const auto& list : drawLists) draws.insert(draws.end(), list.commands.begin(), list.commands.end());
        draws.insert(draws.end(), foreignDraws.commands.begin(), foreignDraws.commands.end());
    }
    ensureDrawCapacity(f, total);
    const auto transparent =
        std::stable_partition(draws.begin(), draws.end(), [](const auto& draw) { return !draw.blend; });
    // Reverse-Z is smaller farther away. Object-level sorting is the basic
    // transparency path; intersecting transparent surfaces need a later solution.
    std::stable_sort(transparent, draws.end(), [](const auto& a, const auto& b) { return a.depth < b.depth; });
    // One draw table serves the scene and every shadow map.
    for (uint32_t i = 0; i < draws.size(); ++i) {
        draws[i].drawIndex = i;
        f.drawData[i] = draws[i].data;
    }
    vk::check(
        vmaFlushAllocation(context->allocator(), f.drawBuffer.allocation, 0, sizeof(DrawData) * draws.size()),
        "vmaFlushAllocation");
}

void Renderer::Impl::recordDraws(VkCommandBuffer cmd, Frame& f) {
    if (gatheredDraws.empty()) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &bindlessSet, 0, nullptr);
    DrawPushConstants push{f.drawAddress, f.frameAddress};
    vkCmdPushConstants(
        cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

    VkBuffer boundIndices = VK_NULL_HANDLE;
    VkPipeline boundPipeline = meshPipeline;
    for (const auto& draw : gatheredDraws) {
        const auto pipeline = draw.mirrored ? mirroredPipelines[(draw.blend ? 2 : 0) + (draw.doubleSided ? 1 : 0)]
                              : draw.blend  ? (draw.doubleSided ? meshBlendDoubleSidedPipeline : meshBlendPipeline)
                                            : (draw.doubleSided ? meshDoubleSidedPipeline : meshPipeline);
        if (pipeline != boundPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            boundPipeline = pipeline;
        }
        if (draw.indexBuffer != boundIndices) {
            vkCmdBindIndexBuffer(cmd, draw.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            boundIndices = draw.indexBuffer;
        }
        vkCmdDrawIndexed(cmd, draw.indexCount, 1, 0, 0, draw.drawIndex);
    }
}

void Renderer::Impl::endFrame() {
    Frame& f = frames[frameIndex];
    VkImage image = swapchain->image(imageIndex);
    VkCommandBuffer cmd = f.cmd;

    vk::check(vkResetCommandPool(context->device(), f.pool, 0), "vkResetCommandPool");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk::check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");

    prepareDraws(f);
    writeFrameData(f);
    buildGraph(cmd, f, image);
    graph.setTimestampPool(f.timestamps);
    graph.execute(cmd);
    f.timedPasses = graph.timedPasses();
    frameGraphText = graph.description();

    vk::check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    ++frameNumber;
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

    if (screenshotRequested) finishScreenshot();
    frameIndex = (frameIndex + 1) % kFramesInFlight;
}

void Renderer::Impl::writeFrameData(Frame& f) {
    FrameData header{};
    header.cameraPosition = cameraPosition;
    header.skyAmbient = lighting.skyAmbient;
    header.groundAmbient = lighting.groundAmbient;
    header.debugView = static_cast<uint32_t>(debugView);
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
    header.shadowViews = f.frameAddress + kShadowViewsOffset;
    std::memcpy(f.frameData, &header, sizeof(header));
    vk::check(
        vmaFlushAllocation(context->allocator(), f.frameBuffer.allocation, 0, VK_WHOLE_SIZE), "vmaFlushAllocation");
}

void Renderer::Impl::buildGraph(VkCommandBuffer, Frame& f, VkImage swapchainImage) {
    graph.reset();
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

    std::vector<vk::RenderGraph::Use> sceneUses = {
        {color, ImageUse::ColorAttachment}, {depth, ImageUse::DepthAttachment}};
    if (!cascadePasses.empty()) {
        const auto atlas =
            graph.import("sun shadows", cascadeAtlas.image, VK_IMAGE_ASPECT_DEPTH_BIT, cascadeState, true);
        graph.addPass("shadows (sun)", {{atlas, ImageUse::DepthAttachment}}, [this, &f](VkCommandBuffer cmd) {
            recordShadows(cmd, f, cascadePasses, cascadeAtlas);
        });
        sceneUses.push_back({atlas, ImageUse::Sampled});
    }
    if (!localPasses.empty()) {
        const auto atlas = graph.import("light shadows", localAtlas.image, VK_IMAGE_ASPECT_DEPTH_BIT, localState, true);
        graph.addPass("shadows (lights)", {{atlas, ImageUse::DepthAttachment}}, [this, &f](VkCommandBuffer cmd) {
            recordShadows(cmd, f, localPasses, localAtlas);
        });
        sceneUses.push_back({atlas, ImageUse::Sampled});
    }
    graph.addPass("scene", sceneUses, [this, &f](VkCommandBuffer cmd) { recordScene(cmd, f); });
    graph.addPass(
        "tonemap", {{color, ImageUse::Sampled}, {display, ImageUse::ColorAttachment}},
        [this](VkCommandBuffer cmd) { recordTonemap(cmd); });
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

void Renderer::Impl::recordScene(VkCommandBuffer cmd, Frame& f) {
    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = sceneColor.view;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = {{clearColor.r, clearColor.g, clearColor.b, clearColor.a}};

    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = sceneDepth.view;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
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
    recordDraws(cmd, f);
    recordGrid(cmd);
    vkCmdEndRendering(cmd);
}

void Renderer::Impl::recordTonemap(VkCommandBuffer cmd) {
    VkRenderingAttachmentInfo target{};
    target.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    target.imageView = displayColor.view;
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
    vkCmdPushConstants(cmd, tonemapLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void Renderer::Impl::recordCopyToWindow(VkCommandBuffer cmd, VkImage swapchainImage) {
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
