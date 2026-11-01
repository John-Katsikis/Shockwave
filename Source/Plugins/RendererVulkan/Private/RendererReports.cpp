// What the device can do, what the settings really are, and where GPU memory goes:
// Renderer::capabilities, effectiveSettings and memoryReport (docs/GraphicsVerification.md).

#include "RendererImpl.h"

#include <Core/Log.h>

#include <cstdio>

namespace shockwave {

namespace {

std::string version(uint32_t v) {
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
           std::to_string(VK_API_VERSION_PATCH(v));
}

const char* onOff(bool on) { return on ? "on" : "off"; }

const char* shadowName(ShadowQuality quality) {
    constexpr const char* names[] = {"off", "low", "medium", "high", "ultra"};
    return names[std::min<uint32_t>(static_cast<uint32_t>(quality), 4)];
}

}   // namespace

void VulkanRenderer::Impl::setView(RenderViewId id) {
    if (id == currentView) return;
    // A different view: nothing a temporal effect kept belongs to it.
    currentView = id;
    ++viewChanges;
}

RendererCapabilities VulkanRenderer::Impl::capabilities() const {
    RendererCapabilities caps;
    caps.backend = "vulkan";
    VkPhysicalDeviceVulkan12Properties props12{};
    props12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &props12;
    vkGetPhysicalDeviceProperties2(context->physicalDevice(), &props);
    const VkPhysicalDeviceLimits& limits = props.properties.limits;
    caps.device = props.properties.deviceName;
    caps.driver =
        std::string(props12.driverName) + (props12.driverInfo[0] ? std::string(" ") + props12.driverInfo : "");
    caps.api = "Vulkan " + version(props.properties.apiVersion);

    // Formats later effects need, with the usage each needs. Vulkan 1.3 guarantees some of
    // these, not all: a missing one disables its effect with this as the reason.
    struct FormatNeed {
        const char* name;
        VkFormat format;
        VkFormatFeatureFlags features;
        const char* usedBy;
    };
    const FormatNeed formats[] = {
        {"rgba16f render target with blending", kSceneColorFormat,
         VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
         "the HDR scene, particles"},
        {"rgba16f storage image", VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
         "bloom, environment filtering, TAA"},
        {"rg16f render target", VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "motion vectors"},
        {"r8 storage image", VK_FORMAT_R8_UNORM, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "ambient occlusion"},
        {"r16f storage image", VK_FORMAT_R16_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "ambient occlusion, fog"},
        {"r32f storage image", VK_FORMAT_R32_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "depth pyramid (HZB)"},
        {"d32 sampled depth", kDepthFormat, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
         "shadows, depth prepass readers, soft particles"},
        {"rgba8 srgb render target with blending", kDisplayFormat, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT,
         "the display image, game UI"},
        {"rgba8 storage image", VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "FXAA in compute"},
    };
    for (const FormatNeed& need : formats) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(context->physicalDevice(), need.format, &properties);
        const bool ok = (properties.optimalTilingFeatures & need.features) == need.features;
        caps.features.push_back({need.name, ok, std::string("for ") + need.usedBy});
    }
    caps.features.push_back(
        {"multi draw indirect", context->multiDrawIndirect(), "many draws per call (GPU-driven drawing)"});
    caps.features.push_back(
        {"draw indirect first instance", context->drawIndirectFirstInstance(), "instance data in indirect draws"});
    caps.features.push_back(
        {"draw indirect count", context->drawIndirectCount(), "GPU-chosen draw counts (occlusion culling)"});
    caps.features.push_back({"bc texture compression", context->textureCompressionBC(), "BC1-BC7 textures"});
    char anisotropy[32];
    std::snprintf(anisotropy, sizeof(anisotropy), "up to %gx", static_cast<double>(context->maxAnisotropy()));
    caps.features.push_back({"anisotropic filtering", context->maxAnisotropy() > 1.0f, anisotropy});
    caps.features.push_back(
        {"gpu timestamps", context->timestampPeriod() > 0.0f,
         context->timestampPeriod() > 0.0f ? "per-pass GPU time" : "the graphics queue has none: no pass timings"});
    caps.features.push_back(
        {"memory budget", context->memoryBudget(),
         context->memoryBudget() ? "the driver's numbers" : "estimated from Shockwave's own allocations"});
    caps.features.push_back(
        {"present wait", context->presentWait() != vk::Context::PresentWait::None, context->presentTimingName()});
    caps.features.push_back({"fifo latest ready", context->fifoLatestReady(), "low-latency vsync"});

    caps.limits = {
        {"max image size", limits.maxImageDimension2D},
        {"bindless textures", textureCapacity()},
        {"spare texture slots", spareTextureSlots()},
        {"material samplers", kMaxSamplers},
        {"max storage buffer range", limits.maxStorageBufferRange},
        {"max push constants", limits.maxPushConstantsSize},
        {"max compute shared memory", limits.maxComputeSharedMemorySize},
        {"max compute invocations", limits.maxComputeWorkGroupInvocations},
        {"max storage images per stage", limits.maxPerStageDescriptorStorageImages},
        {"max color attachments", limits.maxColorAttachments},
        {"max draw indirect count", limits.maxDrawIndirectCount},
        {"timed passes", vk::RenderGraph::kMaxTimedPasses},
        {"lights per frame", kMaxLights},
    };
    return caps;
}

std::vector<EffectiveSetting> VulkanRenderer::Impl::effectiveSettings() const {
    std::vector<EffectiveSetting> settings;
    char requested[32], effective[32];
    auto imageBytes = [&](const vk::Image& image) -> uint64_t {
        if (!image.allocation) return 0;
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(context->allocator(), image.allocation, &info);
        return info.size;
    };

    // Render scale: the scene target is whole pixels, so the effective scale is what they
    // came to.
    const VkExtent2D out = outputExtent();
    const float actual = out.width ? static_cast<float>(sceneColor.extent.width) / static_cast<float>(out.width) : 0;
    std::snprintf(requested, sizeof(requested), "%g", static_cast<double>(renderScale));
    std::snprintf(effective, sizeof(effective), "%.3g", static_cast<double>(actual));
    std::string why;
    if (std::abs(actual - renderScale) > 0.01f)
        why = sceneColor.extent.width ? "rounded to " + std::to_string(sceneColor.extent.width) + "x" +
                                            std::to_string(sceneColor.extent.height) + " pixels"
                                      : "no frame drawn yet";
    settings.push_back({"render_scale", requested, effective, why, ""});

    // Shadows: what the tier comes to (RendererShadows.cpp shadowConfig) and costs.
    {
        const bool pending = needsShadowRebuild;
        std::string detail;
        if (shadowQuality != ShadowQuality::Off && !pending) {
            char text[256];
            std::snprintf(
                text, sizeof(text),
                "sun: 2x2 atlas of %u px cascades, %u drawn last frame; lights: %u px atlas of %u px tiles, %u "
                "shadowed, %u didn't fit; %u filter taps; %.1f MiB",
                shadowSettings.cascadeSize, shadowStats.cascades, shadowSettings.localAtlasSize,
                shadowSettings.tileSize, shadowStats.shadowedLights, shadowStats.skippedLights, shadowSettings.taps,
                static_cast<double>(imageBytes(cascadeAtlas) + imageBytes(localAtlas)) / (1024.0 * 1024.0));
            detail = text;
        }
        settings.push_back(
            {"shadow_quality", shadowName(shadowQuality),
             pending ? shadowName(builtShadowQuality) : shadowName(shadowQuality),
             pending ? "pending: the new shadow maps are made once the old ones are out of use" : "", detail});
    }

    // Anti-aliasing: FXAA needs its input image, TAA its motion, reactive and history
    // images (G10), each made at the next frame boundary.
    if (antiAliasing != AntiAliasing::Taa) {
        const bool wanted = antiAliasing == AntiAliasing::Fxaa;
        const bool have = aaTarget.image.image != VK_NULL_HANDLE;
        std::string why = wanted && !have ? "pending: its image is made at the next frame" : "";
        if (wanted && have && !fxaaActive()) why = "off in debug views, which show raw values";
        char detail[128] = "";
        if (have)
            std::snprintf(
                detail, sizeof(detail), "after tone mapping, before game UI; input %ux%u, %.1f MiB",
                aaTarget.image.extent.width, aaTarget.image.extent.height,
                static_cast<double>(imageBytes(aaTarget.image)) / (1024.0 * 1024.0));
        settings.push_back(
            {"anti_aliasing", antiAliasingName(antiAliasing),
             antiAliasingName(have && fxaaActive() ? AntiAliasing::Fxaa : AntiAliasing::None), why, detail});
    } else {
        const bool have = temporalActive();
        const bool lit =
            debugView == DebugView::Lit || debugView == DebugView::Lighting || debugView == DebugView::ShadowCascades;
        std::string why = !have ? "pending: its images are made at the next frame" : "";
        if (have && !lit) why = "off in debug views, which show raw values";
        if (have && lit && !currentView.valid())
            why = "off for a camera without a RenderViewId: it has no history to keep";
        char detail[192] = "";
        if (have) {
            uint64_t bytes = imageBytes(temporalTargets.motion) + imageBytes(temporalTargets.reactive);
            for (int i = 0; i < 2; ++i)
                bytes += imageBytes(temporalTargets.color[i]) + imageBytes(temporalTargets.distance[i]);
            std::snprintf(
                detail, sizeof(detail), "at %ux%u before bloom, jitter cycle %u; last reset: %s (%u so far); %.1f MiB",
                temporalTargets.motion.extent.width, temporalTargets.motion.extent.height, kTaaJitterCycle,
                historyResetName(lastTemporalStats.reset), lastTemporalStats.resets,
                static_cast<double>(bytes) / (1024.0 * 1024.0));
        }
        settings.push_back(
            {"anti_aliasing", "taa",
             antiAliasingName(have && lit && currentView.valid() ? AntiAliasing::Taa : AntiAliasing::None), why,
             detail});
    }

    // Bloom: the chain in use, which lags the request by a frame (or two, when both slots
    // are still in flight).
    {
        const BloomQuality have = bloom.image.image ? bloom.quality : BloomQuality::Off;
        std::string detail;
        if (bloom.image.image) {
            char text[160];
            std::snprintf(
                text, sizeof(text), "%zu levels from %ux%u; %.1f MiB", bloom.extents.size(), bloom.extents[0].width,
                bloom.extents[0].height, static_cast<double>(imageBytes(bloom.image)) / (1024.0 * 1024.0));
            detail = text;
        }
        settings.push_back(
            {"bloom_quality", bloomQualityName(bloomQuality), bloomQualityName(have),
             have == bloomQuality ? "" : "pending: the new bloom chain is made at the next frame", detail});
        char requested[32];
        std::snprintf(requested, sizeof(requested), "%g", static_cast<double>(bloomScale));
        const float authored = std::clamp(authoredBloom(), 0.0f, 1.0f);
        // Not a fallback when bloom is off: the scale just has nothing to scale.
        char detail2[128];
        if (bloom.image.image)
            std::snprintf(
                detail2, sizeof(detail2), "scene intensity %g x %g = %g", static_cast<double>(authored),
                static_cast<double>(bloomScale), static_cast<double>(bloomIntensity()));
        else
            std::snprintf(detail2, sizeof(detail2), "unused while bloom_quality is off");
        settings.push_back({"bloom_scale", requested, requested, "", detail2});
    }

    // Ambient occlusion (G04), with the surface prepass it needs (G03).
    {
        const AmbientOcclusion effective = effectiveAmbientOcclusion();
        const bool have = prepassActive();
        std::string why;
        if (effective != AmbientOcclusion::Off && !have)
            why = "pending: its images are made at the next frame";
        else if (effective != ambientOcclusion)
            why = "the ambient occlusion debug view computes it at medium";
        char detail[200] = "";
        if (have) {
            const VkExtent2D half = aoTargets.ao[0].image ? aoTargets.ao[0].extent : VkExtent2D{0, 0};
            uint64_t bytes = imageBytes(aoTargets.normal);
            for (const vk::Image& image : aoTargets.ao) bytes += imageBytes(image);
            std::snprintf(
                detail, sizeof(detail),
                "%u samples at %ux%u (half of %ux%u), radius %g m; surface prepass with %s normals; %.1f MiB",
                ambientOcclusionSamples(effective), half.width, half.height, sceneColor.extent.width,
                sceneColor.extent.height, static_cast<double>(lighting.aoRadius),
                aoNormalFormat == VK_FORMAT_R16G16_SNORM ? "rg16 snorm" : "rg16f",
                static_cast<double>(bytes) / (1024.0 * 1024.0));
        } else {
            std::snprintf(detail, sizeof(detail), "no surface prepass");
        }
        settings.push_back(
            {"ambient_occlusion", ambientOcclusionName(ambientOcclusion),
             ambientOcclusionName(have ? effective : AmbientOcclusion::Off), why, detail});
    }

    // Decals (G08): their buffers and the prepass's layer target, while there are any.
    {
        const bool have = decalsActive();
        std::string why;
        if (decalQuality != DecalQuality::Off && !have)
            why = lastSubmittedDecals ? "pending: its images are made at the next frame"
                                      : "no decals in the scene: no decal pass or buffers";
        char detail[200] = "";
        const DecalStats& s = lastDecalStats;
        const uint64_t bytes =
            imageBytes(decalTargets.layers) + imageBytes(decalTargets.albedo) + imageBytes(decalTargets.material);
        std::snprintf(
            detail, sizeof(detail),
            "budget %u; last frame %u submitted, %u visible, %u drawn, %u over budget; %.1f MiB",
            decalBudget(decalQuality), s.submitted, s.visible, s.drawn, s.overBudget,
            static_cast<double>(bytes) / (1024.0 * 1024.0));
        settings.push_back(
            {"decals_quality", decalQualityName(decalQuality),
             decalQualityName(have ? decalQuality : DecalQuality::Off), why, detail});
    }

    // Clustered lighting (G11): whether last frame built lists, and what they came to.
    {
        const ClusterStats& c = lastClusterStats;
        const LightCulling asked = lightCulling.load(std::memory_order_relaxed);
        char detail[256];
        if (c.clustered)
            std::snprintf(
                detail, sizeof(detail),
                "%ux%u-pixel tiles, %ux%ux%u clusters to %g m; %u point and spot lights, up to %u a cluster "
                "(%.1f on average), %u overflowed; %.2f MiB",
                c.grid.tileSize, c.grid.tileSize, c.grid.tilesX, c.grid.tilesY, c.grid.slices,
                static_cast<double>(c.farPlane), c.localLights, c.maxCandidates,
                static_cast<double>(c.averageCandidates), c.overflowClusters,
                static_cast<double>(c.listBytes) / (1024.0 * 1024.0));
        else
            std::snprintf(
                detail, sizeof(detail), "%u point and spot lights, each tested by every pixel (%s)", c.localLights,
                c.reason);
        // Brute force asked for, or auto choosing it, isn't a fallback; clustered not clustering is.
        settings.push_back(
            {"light_culling", lightCullingName(asked), c.clustered ? "clustered" : "brute_force",
             asked == LightCulling::Clustered && !c.clustered ? c.reason : "", detail});
    }

    // Particles (A04): what last frame drew of soft particles, distortion and scene lighting.
    {
        const ParticleStats& p = lastParticleStats;
        const bool soft = softParticles.load(std::memory_order_relaxed);
        const bool distortion = particleDistortion.load(std::memory_order_relaxed);
        const ParticleLighting lighting = particleLighting.load(std::memory_order_relaxed);
        char detail[160];
        std::snprintf(
            detail, sizeof(detail), "last frame %u of %u particle draws faded against scene depth%s", p.softDraws,
            p.draws, p.depthPass ? "; depth-writing ones drawn first" : "");
        settings.push_back({"soft_particles", onOff(soft), onOff(soft), "", detail});
        std::snprintf(
            detail, sizeof(detail), "last frame %u distortion draws, %u left out; scene copy %.1f MiB",
            p.distortionDraws, p.skippedDistortion,
            static_cast<double>(imageBytes(particleBackground.image)) / (1024.0 * 1024.0));
        settings.push_back({"particle_distortion", onOff(distortion), onOff(distortion), "", detail});
        std::snprintf(detail, sizeof(detail), "last frame %u lit particle draws took every light", p.sceneLitDraws);
        settings.push_back(
            {"particle_lighting", particleLightingName(lighting), particleLightingName(lighting), "", detail});
    }

    // Character and specialized shading (A07-A09): what last frame drew.
    {
        const ShadingStats& st = lastShadingStats;
        const bool coat = shadingClearcoat.load(std::memory_order_relaxed);
        const TransmissionQuality glass = shadingTransmission.load(std::memory_order_relaxed);
        const SkinQuality skin = shadingSkin.load(std::memory_order_relaxed);
        const HairQuality hair = shadingHair.load(std::memory_order_relaxed);
        char detail[200];
        std::snprintf(detail, sizeof(detail), "last frame %u coated draws", st.coatDraws);
        settings.push_back({"clearcoat", onOff(coat), onOff(coat), "", detail});
        std::snprintf(
            detail, sizeof(detail),
            "last frame %u glass draws through the transmission pass, %u simplified; copy %.1f MiB",
            st.transmissionDraws, st.simplifiedGlassDraws,
            static_cast<double>(st.transmissionBytes) / (1024.0 * 1024.0));
        settings.push_back(
            {"transmission_quality", transmissionQualityName(glass), transmissionQualityName(glass), "", detail});
        std::snprintf(
            detail, sizeof(detail), "last frame %u skin draws diffused (%u taps); targets %.1f MiB", st.skinDraws,
            skinBlurTaps(skin), static_cast<double>(st.skinBytes) / (1024.0 * 1024.0));
        const bool skinView = debugView == DebugView::Lit || debugView == DebugView::Lighting;
        settings.push_back(
            {"skin_quality", skinQualityName(skin), skinView ? skinQualityName(skin) : "off",
             skin != SkinQuality::Off && !skinView ? "off in debug views other than lit and lighting" : "", detail});
        std::snprintf(
            detail, sizeof(detail), "last frame %u hair card draws%s", st.hairDraws,
            st.anisotropicHair ? ", with the stretched highlight" : "");
        settings.push_back({"hair_quality", hairQualityName(hair), hairQualityName(hair), "", detail});
    }

    // Colour grading, exposure and fog (A05, A06): what last frame applied.
    {
        const PostProcessStats& p = lastPostProcessStats;
        const PostProcessSettings& asked = postProcessSettings;
        const bool lit = debugView == DebugView::Lit;
        char detail[256];
        const bool grading = p.whiteBalance || p.luts;
        std::snprintf(
            detail, sizeof(detail), "white balance %s; %u LUT%s%s", p.whiteBalance ? "applied" : "none", p.luts,
            p.luts == 1 ? "" : "s", p.lutsApproximated ? " (more than 2 overlapped: the top two, renormalized)" : "");
        std::string why;
        if (asked.colorGrading && !lit)
            why = "off in debug views, which show raw values";
        else if (asked.colorGrading && !grading)
            why = "no post-process volume grades the camera";
        settings.push_back(
            {"color_grading", onOff(asked.colorGrading), onOff(asked.colorGrading && grading), why,
             asked.colorGrading ? detail : "white balance and LUTs skipped"});
        const bool metering = p.autoExposure > 0.0f;
        if (metering)
            std::snprintf(
                detail, sizeof(detail),
                "%.0f%% metered; exposure %.2f EV, target %.2f EV%s; %.1f%% outside the "
                "histogram; %u resets; %.1f KiB",
                static_cast<double>(p.autoExposure * 100.0f), static_cast<double>(p.exposure),
                static_cast<double>(p.meteredTarget), p.meteredValid ? "" : " (nothing metered yet)",
                static_cast<double>(p.clampedFraction * 100.0f), p.exposureResets,
                static_cast<double>(p.exposureBytes) / 1024.0);
        else
            std::snprintf(detail, sizeof(detail), "manual exposure %.2f EV", static_cast<double>(p.exposure));
        why.clear();
        if (asked.autoExposure && !metering)
            why = captureRequested ? "reflection captures use manual exposure"
                                   : "no post-process volume asks for auto exposure";
        settings.push_back({"auto_exposure", onOff(asked.autoExposure), onOff(metering), why, detail});
        if (p.fogQuality >= FogQuality::Low)
            std::snprintf(
                detail, sizeof(detail), "%ux%ux%u froxels of %u px to %g m, analytic past it; %.2f MiB",
                p.froxels.tilesX, p.froxels.tilesY, p.froxels.slices, p.froxels.tileSize,
                static_cast<double>(postProcess.fog.volumetricDistance),
                static_cast<double>(p.fogBytes) / (1024.0 * 1024.0));
        else if (p.fogQuality == FogQuality::Analytic)
            std::snprintf(detail, sizeof(detail), "closed form per pixel, sun unshadowed; no buffers");
        else
            std::snprintf(detail, sizeof(detail), "no fog pass or buffers");
        settings.push_back(
            {"fog_quality", fogQualityName(p.fogRequested), fogQualityName(p.fogQuality),
             p.fogQuality == p.fogRequested ? "" : p.fogReason, detail});
    }

    // Vsync and low latency come down to the present mode the surface allowed.
    const VkPresentModeKHR mode = swapchain->presentMode();
    const bool synced = mode == VK_PRESENT_MODE_FIFO_KHR || mode == VK_PRESENT_MODE_FIFO_LATEST_READY_KHR ||
                        mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    settings.push_back(
        {"vsync", onOff(swapchain->vsync()), onOff(synced),
         synced == swapchain->vsync() ? ""
         : synced                     ? "the display has no present mode without vsync, so FIFO it is"
                                      : "the driver chose a present mode without vsync",
         ""});
    const bool latestReady = mode == VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
    const bool lowLatency = swapchain->lowLatency();
    std::string latency = onOff(lowLatency);
    if (lowLatency)
        latency += std::string(" (") + context->presentTimingName() + (latestReady ? ", latest ready)" : ")");
    settings.push_back(
        {"low_latency", onOff(lowLatency), latency,
         lowLatency && synced && !latestReady ? "no FIFO latest ready: waits for the GPU before input instead" : "",
         ""});
    // Anisotropy: what the setting asks for, capped by the device. A texture asking for less,
    // or for nearest filtering, gets that (RendererMaterials.cpp).
    {
        const float device = context->maxAnisotropy();
        const float effectiveValue = std::min(float(anisotropy), std::max(device, 1.0f));
        std::snprintf(requested, sizeof(requested), "%u", anisotropy);
        std::snprintf(effective, sizeof(effective), "%g", static_cast<double>(effectiveValue));
        char detail[96];
        std::snprintf(
            detail, sizeof(detail), "%zu material samplers; device limit %gx", samplerDescs.size(),
            static_cast<double>(device));
        settings.push_back(
            {"anisotropy", requested, effective, effectiveValue < float(anisotropy) ? "the GPU's limit is lower" : "",
             detail});
    }
    settings.push_back(
        {"gpu_timing", "on", onOff(context->timestampPeriod() > 0.0f),
         context->timestampPeriod() > 0.0f ? "" : "the graphics queue has no timestamps", ""});
    return settings;
}

void VulkanRenderer::Impl::logSettingFallbacks() {
    // Once per change, never once per frame.
    std::string now;
    for (const EffectiveSetting& setting : effectiveSettings())
        if (!setting.reason.empty() && setting.name != "render_scale")
            now += setting.name + ": " + setting.requested + " -> " + setting.effective + " (" + setting.reason + "); ";
    if (now == loggedFallbacks) return;
    loggedFallbacks = now;
    if (!now.empty()) SW_LOG(LogRender, Warning, "graphics settings fall back: {}", now);
}

RendererMemoryReport VulkanRenderer::Impl::memoryReport() const {
    RendererMemoryReport report;
    VmaAllocator allocator = context->allocator();
    auto add = [&](GpuMemoryCategory category, VmaAllocation allocation) {
        if (!allocation) return;
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(allocator, allocation, &info);
        report[category].bytes += info.size;
        ++report[category].allocations;
    };
    add(GpuMemoryCategory::RenderTargets, sceneColor.allocation);
    add(GpuMemoryCategory::RenderTargets, sceneDepth.allocation);
    add(GpuMemoryCategory::RenderTargets, displayColor.allocation);
    add(GpuMemoryCategory::ShadowMaps, cascadeAtlas.allocation);
    add(GpuMemoryCategory::ShadowMaps, localAtlas.allocation);
    for (const CubeMap* cube : {&environmentCube, &specularCube, &irradianceCube})
        add(GpuMemoryCategory::Environment, cube->allocation);
    add(GpuMemoryCategory::Environment, brdfTable.allocation);
    add(GpuMemoryCategory::Bloom, bloom.image.allocation);
    add(GpuMemoryCategory::AntiAliasing, aaTarget.image.allocation);
    add(GpuMemoryCategory::SurfaceBuffers, aoTargets.normal.allocation);
    for (const vk::Image* image : {&decalTargets.layers, &decalTargets.albedo, &decalTargets.material})
        add(GpuMemoryCategory::SurfaceBuffers, image->allocation);
    for (const vk::Image& image : aoTargets.ao) add(GpuMemoryCategory::AmbientOcclusion, image.allocation);
    // G09, G10: motion vectors, the reactive mask and both histories.
    add(GpuMemoryCategory::TemporalHistory, temporalTargets.motion.allocation);
    add(GpuMemoryCategory::TemporalHistory, temporalTargets.reactive.allocation);
    for (int i = 0; i < 2; ++i) {
        add(GpuMemoryCategory::TemporalHistory, temporalTargets.color[i].allocation);
        add(GpuMemoryCategory::TemporalHistory, temporalTargets.distance[i].allocation);
    }
    for (const Frame& f : frames) {
        add(GpuMemoryCategory::Clusters, f.clusterBuffer.allocation);
        add(GpuMemoryCategory::Clusters, f.clusterCounters.allocation);
    }
    // A05, A06: only once a frame meters exposure or fills the volume.
    add(GpuMemoryCategory::Fog, froxelBuffer.allocation);
    add(GpuMemoryCategory::Exposure, exposureState.allocation);
    for (const Frame& f : frames) add(GpuMemoryCategory::Exposure, f.exposureReadback.allocation);
    add(GpuMemoryCategory::Readback, screenshotBuffer.allocation);
    add(GpuMemoryCategory::Readback, captureBuffer.allocation);
    for (const Frame& f : frames) {
        for (const vk::Buffer* b :
             {&f.drawBuffer, &f.layerBuffer, &f.probeBuffer, &f.shadingBuffer, &f.frameBuffer, &f.jointBuffer,
              &f.skinnedBuffer})
            add(GpuMemoryCategory::FrameData, b->allocation);
        add(GpuMemoryCategory::GameUi, f.uiVertexBuffer.allocation);
        add(GpuMemoryCategory::GameUi, f.uiIndexBuffer.allocation);
    }
    for (const ParticleBuffers& p : particleBuffers)
        for (const vk::Buffer* b : {&p.vertices, &p.indices, &p.batches})
            add(GpuMemoryCategory::Particles, b->allocation);
    for (uint32_t id = 0; id < kMaxMeshes; ++id) {
        if (!meshReady[id].load(std::memory_order_acquire)) continue;
        const Mesh& mesh = meshes[id];
        for (const vk::Buffer* b :
             {&mesh.vertices, &mesh.indices, &mesh.lodIndices, &mesh.skin, &mesh.uv1, &mesh.lightmapUv})
            add(GpuMemoryCategory::Meshes, b->allocation);
    }
    add(GpuMemoryCategory::Particles, particleBackground.image.allocation);   // distortion's scene copy
    add(GpuMemoryCategory::Transmission, transmissionBackground.image.allocation);   // A07b
    for (const vk::Image* image : {&skinTargets.light, &skinTargets.skin, &skinTargets.scratch})
        add(GpuMemoryCategory::Skin, image->allocation);   // A08
    {
        // Textures can be created from worker threads; their descriptor write takes this lock.
        std::lock_guard lock(const_cast<std::mutex&>(descriptorMutex));
        for (uint32_t id = 0; id < textureCapacity(); ++id)
            if (textures[id].image)
                add(cubeSlot(id) != kNoSlot ? GpuMemoryCategory::ReflectionProbes : GpuMemoryCategory::Textures,
                    textures[id].allocation);
    }

    std::vector<VmaBudget> budgets(VK_MAX_MEMORY_HEAPS);
    vmaGetHeapBudgets(allocator, budgets.data());
    const VkPhysicalDeviceMemoryProperties* memory = nullptr;
    vmaGetMemoryProperties(allocator, &memory);
    for (uint32_t heap = 0; memory && heap < memory->memoryHeapCount; ++heap) {
        if (!(memory->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
        report.deviceUsage += budgets[heap].usage;
        report.deviceBudget += budgets[heap].budget;
    }
    return report;
}

}   // namespace shockwave
