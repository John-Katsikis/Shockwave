#include "RendererImpl.h"

#include <Renderer/Clusters.h>

namespace shockwave {

// Colour, exposure and fog (A05, A06; Renderer/PostProcess.h, docs/PostProcess.md and
// docs/Fog.md). Nothing here runs for a scene without postprocess volumes: no metering
// passes, no fog volume, the identity white balance and no LUTs, so its frames are the same
// pixel for pixel as before.
//
//   fog volume (compute, before the scene) -> scene (fog applied per surface, sky, particles)
//   -> TAA -> exposure histogram -> exposure adapt -> bloom -> tonemap (white balance,
//   exposure, curve, LUTs)

namespace {
// Must match the push constants in shaders/exposure.glsl.
struct ExposurePushConstants {
    VkDeviceAddress state;
    VkDeviceAddress readback;
    uint32_t sceneTexture;
    uint32_t metering;
    uint32_t width, height;
    float dt;
    float compensation, minEv, maxEv, targetGray, lowPercentile, highPercentile, brightenTime, darkenTime;
    uint32_t reset;
};
static_assert(sizeof(ExposurePushConstants) == 72, "must match shaders/exposure.glsl");
// Must match the push constants in shaders/fog.comp.
struct FogPushConstants {
    VkDeviceAddress frame;
    VkDeviceAddress froxels;
};
constexpr uint32_t kFogNone = 0, kFogAnalytic = 1, kFogFroxels = 2;

VkPipeline computePipeline(VkDevice device, VkPipelineLayout layout, const std::string& path) {
    VkShaderModule module = vk::loadShaderModule(device, path);
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    vk::check(result, "vkCreateComputePipelines");
    return pipeline;
}
}   // namespace

void VulkanRenderer::Impl::createPostProcessPipelines() {
    VkDevice device = context->device();
    auto layout = [&](uint32_t bytes, VkPipelineLayout& out) {
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, bytes};
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1;
        info.pSetLayouts = &bindlessLayout;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;
        vk::check(vkCreatePipelineLayout(device, &info, nullptr, &out), "vkCreatePipelineLayout");
    };
    layout(sizeof(ExposurePushConstants), exposureLayout);
    exposureHistogramPipeline = computePipeline(device, exposureLayout, shaderPath("exposure_histogram.comp"));
    exposureAdaptPipeline = computePipeline(device, exposureLayout, shaderPath("exposure_adapt.comp"));
    layout(sizeof(FogPushConstants), fogLayout);
    fogPipeline = computePipeline(device, fogLayout, shaderPath("fog.comp"));
}

void VulkanRenderer::Impl::destroyPostProcess() {
    VkDevice device = context->device();
    VmaAllocator allocator = context->allocator();
    if (exposureState.buffer) vmaDestroyBuffer(allocator, exposureState.buffer, exposureState.allocation);
    exposureState = {};
    exposureAddress = 0;
    if (froxelBuffer.buffer) vmaDestroyBuffer(allocator, froxelBuffer.buffer, froxelBuffer.allocation);
    froxelBuffer = {};
    froxelCapacity = 0;
    for (Frame& f : frames) {
        if (f.exposureReadback.buffer)
            vmaDestroyBuffer(allocator, f.exposureReadback.buffer, f.exposureReadback.allocation);
        f.exposureReadback = {};
        f.exposureData = nullptr;
        f.exposurePending = false;
    }
    vkDestroyPipeline(device, exposureHistogramPipeline, nullptr);
    vkDestroyPipeline(device, exposureAdaptPipeline, nullptr);
    vkDestroyPipelineLayout(device, exposureLayout, nullptr);
    vkDestroyPipeline(device, fogPipeline, nullptr);
    vkDestroyPipelineLayout(device, fogLayout, nullptr);
}

float VulkanRenderer::Impl::authoredBloom() const {
    return postProcess.overrideBloom ? postProcess.bloomIntensity : lighting.bloomIntensity;
}

void VulkanRenderer::Impl::preparePostProcess(Frame& f, FrameData& header) {
    VmaAllocator allocator = context->allocator();
    // Frees a buffer once the frames submitted so far are done with it.
    auto retireBuffer = [&](vk::Buffer buffer) {
        retire([allocator, buffer] { vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation); });
    };
    PostProcessStats stats;
    const PostProcessSettings settings = postProcessSettings;
    const bool lit = debugView == DebugView::Lit;

    // ---- Exposure: metered when a profile asks and auto_exposure is on ----
    if (f.exposurePending && f.exposureData) {
        // This slot's last frame is done (beginFrame waited): its metering can be read.
        vk::check(
            vmaInvalidateAllocation(allocator, f.exposureReadback.allocation, 0, sizeof(ExposureReadback)),
            "vmaInvalidateAllocation");
        const ExposureReadback& r = *f.exposureData;
        lastPostProcessStats.meteredValid = r.valid != 0;
        lastPostProcessStats.meteredTarget = r.target;
        lastPostProcessStats.clampedFraction = r.total ? float(double(r.clamped) / double(r.total)) : 0.0f;
        lastPostProcessStats.exposureResets = r.resets;
        meteredEv = r.ev;
        f.exposurePending = false;
    }
    const float autoShare = settings.autoExposure ? std::clamp(postProcess.autoExposure, 0.0f, 1.0f) : 0.0f;
    exposureThisFrame = autoShare > 0.0f && !captureRequested && validAutoExposure(postProcess.autoParams);
    if (currentView != exposureView) {
        exposureView = currentView;
        exposureReset = true;
    }
    if (exposureCut && postProcess.autoParams.resetOnCut) exposureReset = true;
    exposureCut = false;
    if (exposureThisFrame) {
        if (!exposureState.buffer) {
            exposureState = createBuffer(
                kExposureStateBytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                0);
            exposureAddress = bufferAddress(exposureState.buffer);
            exposureBufferState = {};
            exposureReset = true;
            needsExposureClear = true;
        }
        if (!f.exposureReadback.buffer) {
            VmaAllocationInfo info{};
            f.exposureReadback = createBuffer(
                sizeof(ExposureReadback),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, &info);
            f.exposureData = static_cast<const ExposureReadback*>(info.pMappedData);
            f.exposureReadbackAddress = bufferAddress(f.exposureReadback.buffer);
        }
        f.exposurePending = true;
    } else if (exposureState.buffer && !(autoShare > 0.0f)) {
        // Nothing meters any more: the state goes (once no frame uses it), and the next
        // metered frame starts afresh.
        retireBuffer(exposureState);
        exposureState = {};
        exposureAddress = 0;
        for (Frame& frame : frames)
            if (frame.exposureReadback.buffer) {
                retireBuffer(frame.exposureReadback);
                frame.exposureReadback = {};
                frame.exposureData = nullptr;
                frame.exposurePending = false;
            }
    }
    stats.autoExposure = exposureThisFrame ? autoShare : 0.0f;
    const float manual =
        std::clamp(postProcess.overrideExposure ? postProcess.exposure : lighting.exposure, -16.0f, 16.0f);
    stats.exposure = exposureThisFrame ? manual + (meteredEv - manual) * autoShare : manual;
    stats.meteredValid = exposureThisFrame && lastPostProcessStats.meteredValid;
    stats.meteredTarget = lastPostProcessStats.meteredTarget;
    stats.clampedFraction = lastPostProcessStats.clampedFraction;
    stats.exposureResets = lastPostProcessStats.exposureResets;

    // ---- Grading ----
    stats.whiteBalance = settings.colorGrading && lit && validWhitePoint(postProcess.whitePoint) &&
                         (std::abs(postProcess.whitePoint.x - kD65White.x) > 1e-6f ||
                          std::abs(postProcess.whitePoint.y - kD65White.y) > 1e-6f);
    stats.luts = settings.colorGrading && lit ? postProcess.lutCount : 0;
    stats.lutsApproximated = stats.luts && postProcess.lutsApproximated;

    // ---- Fog ----
    const FogDesc& fog = postProcess.fog;
    stats.fogRequested = settings.fog;
    FogQuality quality = settings.fog;
    const float nearPlane = projectionNearPlane(unjitteredProjection);
    if (quality == FogQuality::Off) {
        stats.fogReason = "fog_quality off";
    } else if (!fog.active() || !validFog(fog)) {
        quality = FogQuality::Off;
        stats.fogReason = "the scene has no fog";
    } else if (captureRequested) {
        quality = FogQuality::Off;
        stats.fogReason = "reflection captures are taken without fog";
    } else if (quality >= FogQuality::Low && nearPlane <= 0.0f) {
        quality = FogQuality::Analytic;
        stats.fogReason = "orthographic view: analytic fog";
    }
    header.fogMode = quality == FogQuality::Off        ? kFogNone
                     : quality == FogQuality::Analytic ? kFogAnalytic
                                                       : kFogFroxels;
    header.fogDensity = fog.density;
    header.fogHeightFalloff = fog.heightFalloff;
    header.fogBaseHeight = fog.baseHeight;
    header.fogMaxDensity = fog.maxDensity;
    header.fogColor = fog.color;
    header.fogAlbedo = fog.albedo;
    header.fogAnisotropy = std::clamp(fog.anisotropy, -kMaxFogAnisotropy, kMaxFogAnisotropy);
    header.fogStart = fog.start;
    header.fogSkyDistance = fog.skyDistance;
    header.fogVolumetricDistance = fog.volumetricDistance;
    header.froxels = 0;
    froxelsThisFrame = header.fogMode == kFogFroxels;
    if (froxelsThisFrame) {
        stats.froxels =
            froxelGrid(sceneColor.extent.width, sceneColor.extent.height, quality, nearPlane, fog.volumetricDistance);
        if (stats.froxels.bytes() > froxelCapacity) {
            if (froxelBuffer.buffer) retireBuffer(froxelBuffer);
            froxelBuffer = createBuffer(
                stats.froxels.bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                0);
            froxelAddress = bufferAddress(froxelBuffer.buffer);
            froxelCapacity = stats.froxels.bytes();
            froxelState = {};
        }
        header.froxels = froxelAddress;
        header.froxelTile = stats.froxels.tileSize;
        header.froxelTilesX = stats.froxels.tilesX;
        header.froxelTilesY = stats.froxels.tilesY;
        header.froxelSlices = stats.froxels.slices;
        header.froxelNear = stats.froxels.nearPlane;
        header.fogVolumetricDistance = stats.froxels.farPlane;
    } else if (froxelBuffer.buffer && (quality < FogQuality::Low || settings.fog < FogQuality::Low)) {
        // No volume this frame: freed once the frames using it finish.
        retireBuffer(froxelBuffer);
        froxelBuffer = {};
        froxelAddress = 0;
        froxelCapacity = 0;
    }
    stats.fogQuality = quality;
    stats.fogBytes = froxelCapacity;
    stats.exposureBytes = exposureState.buffer ? kExposureStateBytes : 0;
    for (const Frame& frame : frames)
        if (frame.exposureReadback.buffer) stats.exposureBytes += sizeof(ExposureReadback);
    lastPostProcessStats = stats;
}

void VulkanRenderer::Impl::recordFog(VkCommandBuffer cmd, Frame& f) {
    const FroxelGrid& grid = lastPostProcessStats.froxels;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fogPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fogLayout, 0, 1, &bindlessSet, 0, nullptr);
    FogPushConstants push{f.frameAddress, froxelAddress};
    vkCmdPushConstants(cmd, fogLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (grid.tilesX + 7) / 8, (grid.tilesY + 7) / 8, 1);
}

void VulkanRenderer::Impl::recordExposureHistogram(VkCommandBuffer cmd) {
    if (needsExposureClear) {
        // A new state buffer starts at zero: histogram, exposure and flags.
        vkCmdFillBuffer(cmd, exposureState.buffer, 0, kExposureStateBytes, 0);
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
        needsExposureClear = false;
    }
    ExposurePushConstants push{};
    push.state = exposureAddress;
    push.sceneTexture = sceneSlot;
    push.metering = postProcess.autoParams.metering == ExposureMetering::Center ? 1u : 0u;
    push.width = sceneColor.extent.width;
    push.height = sceneColor.extent.height;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, exposureHistogramPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, exposureLayout, 0, 1, &bindlessSet, 0, nullptr);
    vkCmdPushConstants(cmd, exposureLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    // Every other pixel each way, 16 x 16 a group.
    vkCmdDispatch(cmd, ((push.width + 1) / 2 + 15) / 16, ((push.height + 1) / 2 + 15) / 16, 1);
}

void VulkanRenderer::Impl::recordExposureAdapt(VkCommandBuffer cmd, Frame& f) {
    const AutoExposureDesc& a = postProcess.autoParams;
    ExposurePushConstants push{};
    push.state = exposureAddress;
    push.readback = f.exposureReadbackAddress;
    push.dt = std::isfinite(postProcess.deltaTime) ? postProcess.deltaTime : 0.0f;
    push.compensation = a.compensation;
    push.minEv = a.minEv;
    push.maxEv = a.maxEv;
    push.targetGray = a.targetGray;
    push.lowPercentile = a.lowPercentile;
    push.highPercentile = a.highPercentile;
    push.brightenTime = a.brightenTime;
    push.darkenTime = a.darkenTime;
    push.reset = exposureReset ? 1u : 0u;
    exposureReset = false;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, exposureAdaptPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, exposureLayout, 0, 1, &bindlessSet, 0, nullptr);
    vkCmdPushConstants(cmd, exposureLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, 1, 1, 1);
    // The readback goes to the CPU once the frame is done.
    VkMemoryBarrier2 toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    toHost.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toHost.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &toHost;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void VulkanRenderer::Impl::gradeTonemap(TonemapPushConstants& push) const {
    push.whiteBalance = glm::mat3(1.0f);
    push.manualEv = std::clamp(postProcess.overrideExposure ? postProcess.exposure : lighting.exposure, -16.0f, 16.0f);
    push.exposureScale = std::exp2(push.manualEv);
    if (push.tonemapper == 99u) return;   // debug views: raw values
    if (exposureThisFrame) {
        push.autoExposure = std::clamp(postProcess.autoExposure, 0.0f, 1.0f);
        push.exposure = exposureAddress;
    }
    if (!postProcessSettings.colorGrading || debugView != DebugView::Lit) return;
    push.whiteBalance = whiteBalanceMatrix(postProcess.whitePoint);
    for (uint32_t i = 0; i < std::min(postProcess.lutCount, kMaxActiveLuts); ++i) {
        const PostProcessLut& lut = postProcess.luts[i];
        if (!lut.texture.id || lut.size < 2 || !(lut.weight > 0.0f)) continue;
        push.lutTexture[i] = textureSlot(lut.texture.id);
        push.lutSize[i] = lut.size | (lut.encoding == LutEncoding::Linear ? kLutLinear : 0u);
        push.lutWeight[i] = std::clamp(lut.weight, 0.0f, 1.0f);
    }
}

}   // namespace shockwave
