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

    // Anti-aliasing: FXAA needs its input image, made at the next frame boundary.
    {
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
        const float authored = std::clamp(lighting.bloomIntensity, 0.0f, 1.0f);
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
    add(GpuMemoryCategory::Readback, screenshotBuffer.allocation);
    for (const Frame& f : frames) {
        for (const vk::Buffer* b : {&f.drawBuffer, &f.layerBuffer, &f.frameBuffer, &f.jointBuffer, &f.skinnedBuffer})
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
        for (const vk::Buffer* b : {&mesh.vertices, &mesh.indices, &mesh.lodIndices, &mesh.skin, &mesh.uv1})
            add(GpuMemoryCategory::Meshes, b->allocation);
    }
    {
        // Textures can be created from worker threads; their descriptor write takes this lock.
        std::lock_guard lock(const_cast<std::mutex&>(descriptorMutex));
        for (uint32_t id = 0; id < textureCapacity(); ++id)
            if (textures[id].image) add(GpuMemoryCategory::Textures, textures[id].allocation);
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
