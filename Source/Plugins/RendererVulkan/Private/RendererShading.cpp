// Character and specialized shading (A07-A09, docs/Shading.md, Renderer/Shading.h).
//
// Clearcoat and hair cards are more maths in mesh.glsl, read from a draw's ShadingData:
// nothing else changes, and with clearcoat off or hair_quality low the draw has none of it.
//
// Glass (family transmission) draws see-through. With transmission_quality above off, the
// scene splits after its opaque surfaces: the opaque scene is copied with a full mip chain
// (rough glass reads the blurrier levels), then glass and the other see-through surfaces
// draw back to front, glass over the copy with blending off, working out what it lets
// through itself (mesh.glsl, kTransmissionPass). Off, glass draws as simplified blended
// glass and there's no copy.
//
// Skin (family skin) with skin_quality above off: the scene draws skin's highlights alone;
// the skin pass draws its diffuse light, before its colour, into a target of its own (with
// its colour and a receiver id in another), which skin.frag blurs across then down, never
// from one receiver into another, and adds back times the colour. All of it inside the
// screen rectangle skin covers this frame. Off, skin is an ordinary opaque surface and
// there's no pass.
//
// The copy and the skin targets are made the first frame that needs them and freed when
// their setting is turned off or nothing has needed them for kIdleFrames frames.
#include "RendererImpl.h"

namespace shockwave {

namespace {

constexpr VkFormat kSkinLightFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kSkinFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t kIdleFrames = 300;

// Must match the push constants in shaders/skin.frag.
struct SkinPushConstants {
    VkDeviceAddress frame;
    VkDeviceAddress profiles;
    uint32_t lightTexture;
    uint32_t skinTexture;
    uint32_t depthTexture;
    uint32_t mode;   // 0 blur across, 1 blur down, 2 composite
    uint32_t taps;
    uint32_t pad;
};
static_assert(sizeof(SkinPushConstants) == 40, "SkinPushConstants must match shaders/skin.frag");

uint64_t bytesOf(VmaAllocator allocator, const vk::Image& image) {
    if (!image.allocation) return 0;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(allocator, image.allocation, &info);
    return info.size;
}

}   // namespace

void VulkanRenderer::Impl::setShadingSettings(const ShadingSettings& settings) {
    shadingClearcoat.store(settings.clearcoat, std::memory_order_relaxed);
    shadingTransmission.store(settings.transmission, std::memory_order_relaxed);
    shadingSkin.store(settings.skin, std::memory_order_relaxed);
    shadingHair.store(settings.hair, std::memory_order_relaxed);
}

bool VulkanRenderer::Impl::fillShading(
    const DrawMaterial& material, float scale, DrawCommand& command, ShadingData& out,
    const std::function<bool(const TextureBinding&)>& vertexTangents) {
    const MaterialShading& s = *material.shading;
    out = {};
    out.family = static_cast<uint32_t>(s.family);
    auto slot = [&](TextureHandle t) { return t.id ? textureSlot(t.id) : 0u; };
    bool any = false;
    switch (s.family) {
    case ShadingFamily::Standard:
        // clearcoat off: the surface under the coat, exactly as a standard material.
        if (s.clearcoat > 0.0f && shadingClearcoat.load(std::memory_order_relaxed)) {
            out.flags |= kShadingCoat;
            out.clearcoat = std::clamp(s.clearcoat, 0.0f, 1.0f);
            out.clearcoatRoughness = std::clamp(s.clearcoatRoughness, 0.0f, 1.0f);
            out.clearcoatNormalScale = s.clearcoatNormalScale;
            out.clearcoatTexture = textureSlot(s.clearcoatTexture.id);
            out.clearcoatNormalTexture = slot(s.clearcoatNormalTexture);
            if (out.clearcoatNormalTexture && !vertexTangents(s.bindings[1]))
                out.flags |= kShadingCoatDerivativeTangents;
            any = true;
        }
        break;
    case ShadingFamily::Transmission: {
        // See-through, so drawn with the blended surfaces and never into shadows, the
        // prepass or decals; prepareShading decides its pass.
        command.blend = true;
        command.transmission = true;
        command.castShadows = false;
        out.transmission = std::clamp(s.transmission, 0.0f, 1.0f);
        out.ior = std::clamp(s.ior, 1.0f, 2.5f);
        out.thickness = std::max(s.thickness, 0.0f) * scale;   // the object's scale, once
        out.absorption = absorptionCoefficient(s.attenuationColor, s.attenuationDistance);
        out.transmissionTexture = textureSlot(s.transmissionTexture.id);
        const TransmissionQuality quality = shadingTransmission.load(std::memory_order_relaxed);
        if (s.thin) out.flags |= kShadingThin;
        if (quality >= TransmissionQuality::Medium) out.flags |= kShadingRefract;
        if (quality == TransmissionQuality::High) out.flags |= kShadingRoughTaps;
        any = true;
        break;
    }
    case ShadingFamily::Skin:
        command.skinDiffusion = !material.blend && s.skinStrength > 0.0f;
        out.skinRadius = s.skinRadius * (0.001f * std::clamp(s.skinStrength, 0.0f, 1.0f));   // mm to m, once
        out.skinMaxPixels = std::clamp(s.skinMaxPixels, 1.0f, 64.0f);
        any = true;
        break;
    case ShadingFamily::HairCard:
        if (s.hairAnisotropy != 0.0f && shadingHair.load(std::memory_order_relaxed) == HairQuality::High)
            out.flags |= kShadingAnisotropic;
        // The material's positive stretches the highlight across the strands, the band hair
        // shows (the strands' cross-section scatters it that way); the shader's anisotropy
        // stretches along its tangent, the strand direction, so the sign flips here.
        out.hairAnisotropy = -std::clamp(s.hairAnisotropy, -1.0f, 1.0f);
        out.hairDirection = s.hairDirection == 0 ? 0u : 1u;
        any = true;
        break;
    }
    for (size_t i = 0; i < kShadingSlots; ++i) {
        const TextureBinding& b = s.bindings[i];
        SlotBinding& o = out.slots[i];
        o.offset = b.transform.offset;
        o.scale = b.transform.scale;
        o.rotation = b.transform.rotation;
        o.samplerAndSet = samplerSlot(b.sampler) | (b.uvSet == 1 ? kSlotUvSet1 : 0u);
    }
    return any;
}

void VulkanRenderer::Impl::ensureShadingCapacity(Frame& f, uint32_t count) {
    if (count <= f.shadingCapacity) return;
    uint32_t capacity = std::max<uint32_t>(f.shadingCapacity, 256);
    while (capacity < count) capacity *= 2;
    if (f.shadingBuffer.buffer)
        vmaDestroyBuffer(context->allocator(), f.shadingBuffer.buffer, f.shadingBuffer.allocation);
    VmaAllocationInfo info{};
    f.shadingBuffer = createBuffer(
        capacity * sizeof(ShadingData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, &info);
    f.shadingData = static_cast<ShadingData*>(info.pMappedData);
    f.shadingAddress = bufferAddress(f.shadingBuffer.buffer);
    f.shadingCapacity = capacity;
}

void VulkanRenderer::Impl::prepareShading(Frame& f) {
    ShadingStats stats;
    const TransmissionQuality transmission = shadingTransmission.load(std::memory_order_relaxed);
    const SkinQuality skin = shadingSkin.load(std::memory_order_relaxed);
    const VkExtent2D extent = sceneColor.extent;
    // Skin diffuses in the lit views (and the lighting view), where its diffuse light shows.
    const bool skinView = debugView == DebugView::Lit || debugView == DebugView::Lighting;
    bool anyGlass = false;
    // Where visible skin is on screen: its bounding spheres, projected, grown by the blur's
    // reach. A sphere through the camera's plane covers it all.
    glm::ivec2 lo(INT32_MAX), hi(INT32_MIN);
    const float fy = projection[1][1] * 0.5f * static_cast<float>(extent.height);
    const float fx = projection[0][0] * 0.5f * static_cast<float>(extent.width);
    const bool orthographic = projection[3][3] == 1.0f;
    for (const DrawCommand& draw : gatheredDraws) {
        if (!draw.visible) continue;
        anyGlass |= draw.transmission;
        if (!draw.skinDiffusion || draw.shading == kNoLayers || skin == SkinQuality::Off || !skinView) continue;
        const ShadingData& d = f.shadingData[draw.shading];
        const float margin = d.skinMaxPixels + 2.0f;
        const glm::vec4 center = view * glm::vec4(glm::vec3(draw.bounds), 1.0f);
        const float r = draw.bounds.w;
        if (!orthographic && -center.z - r <= 1e-3f) {
            lo = glm::ivec2(0);
            hi = glm::ivec2(int32_t(extent.width), int32_t(extent.height));
            continue;
        }
        const glm::vec4 clip = projection * center;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        const glm::vec2 pixel(
            (ndc.x * 0.5f + 0.5f) * float(extent.width), (0.5f - ndc.y * 0.5f) * float(extent.height));
        // Measured at the sphere's nearest point, which only makes it larger.
        const float nearest = orthographic ? 1.0f : std::max(-center.z - r, 1e-3f);
        const glm::vec2 radius(r * fx / nearest + margin, r * fy / nearest + margin);
        lo = glm::min(lo, glm::ivec2(glm::floor(pixel - radius)));
        hi = glm::max(hi, glm::ivec2(glm::ceil(pixel + radius)));
    }
    lo = glm::max(lo, glm::ivec2(0));
    hi = glm::min(hi, glm::ivec2(int32_t(extent.width), int32_t(extent.height)));
    const bool anySkin = hi.x > lo.x && hi.y > lo.y;

    // The copy of the scene and the skin targets: kept a while after the last frame that
    // needed them, so glass or skin coming and going doesn't churn memory.
    if (anyGlass && transmission != TransmissionQuality::Off) ensureTransmissionPipelines();
    if (anySkin && skin != SkinQuality::Off) ensureSkinPipelines();
    idleTransmissionFrames = anyGlass ? 0 : idleTransmissionFrames + 1;
    idleSkinFrames = anySkin ? 0 : idleSkinFrames + 1;
    updateTransmissionBackground(
        transmission != TransmissionQuality::Off &&
        (anyGlass || (transmissionBackground.image.image && idleTransmissionFrames < kIdleFrames)));
    updateSkinTargets(
        skin != SkinQuality::Off && (anySkin || (skinTargets.light.image && idleSkinFrames < kIdleFrames)));
    transmissionThisFrame = anyGlass && transmission != TransmissionQuality::Off && transmissionBackground.image.image;
    skinThisFrame = anySkin && skin != SkinQuality::Off && skinTargets.light.image;
    skinRect = {{lo.x, lo.y}, {uint32_t(std::max(hi.x - lo.x, 0)), uint32_t(std::max(hi.y - lo.y, 0))}};

    // The draws' last fields: glass without its pass this frame is the blended stand-in;
    // skin with diffusion gets a receiver id and a profile.
    auto* profiles = reinterpret_cast<SkinProfile*>(f.frameData + kSkinProfilesOffset);
    profiles[0] = {};
    uint32_t receivers = 0;
    for (DrawCommand& draw : gatheredDraws) {
        if (draw.shading == kNoLayers) continue;
        ShadingData& d = f.shadingData[draw.shading];
        const bool counted = draw.visible;
        if (draw.transmission) {
            if (!transmissionThisFrame)
                d.flags = (d.flags & ~(kShadingRefract | kShadingRoughTaps)) | kShadingSimplifiedGlass;
            if (counted) (transmissionThisFrame ? stats.transmissionDraws : stats.simplifiedGlassDraws) += 1;
        }
        const bool diffuses = draw.skinDiffusion && skinThisFrame && draw.visible;
        draw.skinDiffusion = diffuses;   // from here on: drawn in the skin pass
        if (diffuses) {
            d.flags |= kShadingSkinDiffusion;
            d.skinReceiver = receivers % kMaxSkinReceivers + 1;
            profiles[d.skinReceiver] = {d.skinRadius, d.skinMaxPixels};
            ++receivers;
        }
        if (counted) {
            stats.skinDraws += diffuses;
            stats.coatDraws += (d.flags & kShadingCoat) != 0;
            stats.hairDraws += d.family == uint32_t(ShadingFamily::HairCard);
            stats.anisotropicHair |= (d.flags & kShadingAnisotropic) != 0;
        }
    }
    stats.transmissionPass = transmissionThisFrame;
    stats.skinPass = skinThisFrame;
    stats.transmissionBytes = bytesOf(context->allocator(), transmissionBackground.image);
    stats.skinBytes = bytesOf(context->allocator(), skinTargets.light) +
                      bytesOf(context->allocator(), skinTargets.skin) +
                      bytesOf(context->allocator(), skinTargets.scratch);
    lastShadingStats = stats;
}

void VulkanRenderer::Impl::createShadingPipelines() {
    VkDevice device = context->device();
    transmissionSlots[0] = bindlessSlots() - 18;
    transmissionSlots[1] = bindlessSlots() - 19;
    for (uint32_t pair = 0; pair < 2; ++pair)
        for (uint32_t i = 0; i < 3; ++i) skinSlots[pair][i] = bindlessSlots() - 20 - pair * 3 - i;
    static_assert(kInternalTextureSlots == 25, "the shading slots end the internal ones");

    // The blur's layout now; the pipelines the first frame that needs them
    // (ensureTransmissionPipelines, ensureSkinPipelines), so a game without glass or skin
    // never compiles them.
    VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SkinPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &bindlessLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    vk::check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &skinBlurLayout), "vkCreatePipelineLayout");
}

void VulkanRenderer::Impl::ensureTransmissionPipelines() {
    if (transmissionPipelines[0]) return;
    VkDevice device = context->device();
    // Glass over the copy of the scene: blending off, depth tested and never written.
    const VkSpecializationMapEntry entries[3] = {
        {0, 0, sizeof(VkBool32)}, {1, sizeof(VkBool32), sizeof(VkBool32)}, {2, 2 * sizeof(VkBool32), sizeof(VkBool32)}};
    const VkBool32 glass[3] = {VK_TRUE, VK_FALSE, VK_TRUE};   // alpha test, receives AO, transmission pass
    const VkSpecializationInfo glassSpec{3, entries, sizeof(glass), glass};
    vk::GraphicsPipelineDesc desc;
    desc.vertexShader = shaderPath("mesh.vert");
    desc.fragmentShader = shaderPath("mesh.frag");
    desc.colorFormat = kSceneColorFormat;
    desc.depthFormat = kDepthFormat;
    desc.layout = pipelineLayout;
    desc.depthWrite = false;
    desc.fragmentSpecialization = &glassSpec;
    for (int i = 0; i < 4; ++i) {
        desc.frontFace = (i & 2) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        desc.cullMode = (i & 1) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        transmissionPipelines[i] = vk::createGraphicsPipeline(device, desc);
    }
}

void VulkanRenderer::Impl::ensureSkinPipelines() {
    if (skinPipelines[0]) return;
    VkDevice device = context->device();
    const VkSpecializationMapEntry entries[2] = {{0, 0, sizeof(VkBool32)}, {1, sizeof(VkBool32), sizeof(VkBool32)}};
    // The skin pass: its two targets, over the scene's depth (EQUAL, as over the prepass).
    vk::GraphicsPipelineDesc skinDesc;
    skinDesc.vertexShader = shaderPath("mesh.vert");
    skinDesc.fragmentShader = shaderPath("mesh_skin.frag");
    skinDesc.colorFormat = kSkinLightFormat;
    skinDesc.secondColorFormat = kSkinFormat;
    skinDesc.depthFormat = kDepthFormat;
    skinDesc.layout = pipelineLayout;
    skinDesc.depthWrite = false;
    skinDesc.depthCompare = VK_COMPARE_OP_EQUAL;
    for (uint32_t i = 0; i < 8; ++i) {
        // Occlusion and decals as the scene had them: their slots are 0 when they're off.
        const VkBool32 constants[2] = {(i & 4) ? VK_TRUE : VK_FALSE, VK_TRUE};
        const VkSpecializationInfo spec{2, entries, sizeof(constants), constants};
        skinDesc.frontFace = (i & 2) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        skinDesc.cullMode = (i & 1) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        skinDesc.fragmentSpecialization = &spec;
        skinPipelines[i] = vk::createGraphicsPipeline(device, skinDesc);
    }

    // The blur and the composite: full-screen triangles (inside the skin's rectangle).
    vk::GraphicsPipelineDesc blur;
    blur.vertexShader = shaderPath("tonemap.vert");
    blur.fragmentShader = shaderPath("skin.frag");
    blur.colorFormat = kSkinLightFormat;
    blur.layout = skinBlurLayout;
    blur.cullMode = VK_CULL_MODE_NONE;
    skinBlurPipeline = vk::createGraphicsPipeline(device, blur);
    blur.colorFormat = kSceneColorFormat;
    blur.additiveBlend = true;
    skinCompositePipeline = vk::createGraphicsPipeline(device, blur);
}

void VulkanRenderer::Impl::destroyShadingPipelines() {
    VkDevice device = context->device();
    for (VkPipeline& p : transmissionPipelines) {
        if (p) vkDestroyPipeline(device, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    for (VkPipeline& p : skinPipelines) {
        if (p) vkDestroyPipeline(device, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    if (skinBlurPipeline) vkDestroyPipeline(device, skinBlurPipeline, nullptr);
    if (skinCompositePipeline) vkDestroyPipeline(device, skinCompositePipeline, nullptr);
    if (skinBlurLayout) vkDestroyPipelineLayout(device, skinBlurLayout, nullptr);
    skinBlurPipeline = skinCompositePipeline = VK_NULL_HANDLE;
    skinBlurLayout = VK_NULL_HANDLE;
    destroyTransmissionBackground(transmissionBackground);
    destroySkinTargets(skinTargets);
}

void VulkanRenderer::Impl::destroyTransmissionBackground(TransmissionBackground& background) {
    destroyImage(background.image);
    background = {};
}

void VulkanRenderer::Impl::destroySkinTargets(SkinTargets& targets) {
    destroyImage(targets.light);
    destroyImage(targets.skin);
    destroyImage(targets.scratch);
    targets = {};
}

void VulkanRenderer::Impl::updateTransmissionBackground(bool want) {
    const VkExtent2D extent = sceneColor.extent;
    TransmissionBackground& current = transmissionBackground;
    const bool fits = current.image.image && current.image.extent.width == extent.width &&
                      current.image.extent.height == extent.height;
    if (want == static_cast<bool>(current.image.image) && (!want || fits)) return;
    auto retireCurrent = [this, &current] {
        if (!current.image.image) return;
        const int slot = current.slot == transmissionSlots[0] ? 0 : 1;
        transmissionSlotBusy[slot] = true;
        retire([this, old = current, slot]() mutable {
            destroyTransmissionBackground(old);
            transmissionSlotBusy[slot] = false;
        });
        current = {};
    };
    if (!want) {
        retireCurrent();
        return;
    }
    int free = -1;
    for (int i = 0; i < 2 && free < 0; ++i)
        if (!transmissionSlotBusy[i] && !(current.image.image && transmissionSlots[i] == current.slot)) free = i;
    if (free < 0) return;   // both in flight: the next frame tries again
    TransmissionBackground next;
    next.slot = transmissionSlots[free];
    next.levels = 1;
    for (uint32_t size = std::max(extent.width, extent.height); size > 1; size /= 2) ++next.levels;
    next.image = createImage(
        extent, kSceneColorFormat,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, next.levels, true);
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
    current = next;
}

void VulkanRenderer::Impl::updateSkinTargets(bool want) {
    const VkExtent2D extent = sceneColor.extent;
    SkinTargets& current = skinTargets;
    const bool fits = current.light.image && current.light.extent.width == extent.width &&
                      current.light.extent.height == extent.height;
    if (want == static_cast<bool>(current.light.image) && (!want || fits)) return;
    auto retireCurrent = [this, &current] {
        if (!current.light.image) return;
        const int pair = current.pair;
        skinSlotBusy[pair] = true;
        retire([this, old = current, pair]() mutable {
            destroySkinTargets(old);
            skinSlotBusy[pair] = false;
        });
        current = {};
    };
    if (!want) {
        retireCurrent();
        return;
    }
    int free = -1;
    for (int i = 0; i < 2 && free < 0; ++i)
        if (!skinSlotBusy[i] && !(current.light.image && current.pair == i)) free = i;
    if (free < 0) return;
    SkinTargets next;
    next.pair = free;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    next.light = createImage(extent, kSkinLightFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT, 1, true);
    next.skin = createImage(extent, kSkinFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT, 1, true);
    next.scratch = createImage(extent, kSkinLightFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT, 1, true);
    const VkImageView views[3] = {next.light.view, next.skin.view, next.scratch.view};
    {
        std::lock_guard lock(descriptorMutex);
        for (uint32_t i = 0; i < 3; ++i) {
            next.slots[i] = skinSlots[free][i];
            VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = bindlessSet;
            write.dstBinding = 0;
            write.dstArrayElement = next.slots[i];
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            write.pImageInfo = &imageInfo;
            vkUpdateDescriptorSets(context->device(), 1, &write, 0, nullptr);
        }
    }
    retireCurrent();
    current = next;
    skinLightState = skinState = skinScratchState = {};   // new images start undefined
}

void VulkanRenderer::Impl::recordTransmissionCopy(VkCommandBuffer cmd) {
    // The copy keeps its own barriers, like bloom's chain: only this pass writes it and only
    // the glass after it reads it. The last frame's reads finished before these writes.
    const VkImage image = transmissionBackground.image.image;
    const uint32_t levels = transmissionBackground.levels;
    transitionImage(
        cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, levels);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {sceneColor.extent.width, sceneColor.extent.height, 1};
    vkCmdCopyImage(
        cmd, sceneColor.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
        &region);
    int32_t w = static_cast<int32_t>(sceneColor.extent.width), h = static_cast<int32_t>(sceneColor.extent.height);
    for (uint32_t level = 1; level < levels; ++level) {
        transitionImage(
            cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, level - 1, 1);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        blit.srcOffsets[1] = {w, h, 1};
        w = std::max(w / 2, 1);
        h = std::max(h / 2, 1);
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.dstOffsets[1] = {w, h, 1};
        vkCmdBlitImage(
            cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
            VK_FILTER_LINEAR);
    }
    if (levels > 1)
        transitionImage(
            cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            0, levels - 1);
    transitionImage(
        cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, levels - 1, 1);
}

void VulkanRenderer::Impl::recordSkin(VkCommandBuffer cmd, Frame& f) {
    VkRenderingAttachmentInfo colors[2]{};
    const VkImageView views[2] = {skinTargets.light.view, skinTargets.skin.view};
    for (int i = 0; i < 2; ++i) {
        colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colors[i].imageView = views[i];
        colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;   // receiver 0: not skin
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colors[i].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    }
    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = sceneDepth.view;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = skinRect;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 2;
    rendering.pColorAttachments = colors;
    rendering.pDepthAttachment = &depthAttachment;
    vkCmdBeginRendering(cmd, &rendering);
    setSceneViewport(cmd);
    vkCmdSetScissor(cmd, 0, 1, &skinRect);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &bindlessSet, 0, nullptr);
    DrawPushConstants push{f.drawAddress, f.frameAddress};
    vkCmdPushConstants(
        cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    VkPipeline bound = VK_NULL_HANDLE;
    VkBuffer boundIndices = VK_NULL_HANDLE;
    for (const DrawCommand& draw : gatheredDraws) {
        if (!draw.skinDiffusion) continue;   // prepareShading left only the diffusing ones
        const VkPipeline pipeline = skinPipelines[prepassIndex(draw)];
        if (pipeline != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound = pipeline;
        }
        if (draw.indexBuffer != boundIndices) {
            vkCmdBindIndexBuffer(cmd, draw.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            boundIndices = draw.indexBuffer;
        }
        vkCmdDrawIndexed(cmd, draw.indexCount, 1, draw.firstIndex, 0, draw.drawIndex);
    }
    vkCmdEndRendering(cmd);
}

void VulkanRenderer::Impl::recordSkinBlur(VkCommandBuffer cmd, Frame& f, uint32_t mode) {
    VkRenderingAttachmentInfo target{};
    target.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    target.imageView = mode == 0 ? skinTargets.scratch.view : mode == 1 ? skinTargets.light.view : sceneColor.view;
    target.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    target.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // outside the rectangle: kept
    target.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = skinRect;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &target;
    vkCmdBeginRendering(cmd, &rendering);
    const VkExtent2D extent = sceneColor.extent;
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &skinRect);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mode == 2 ? skinCompositePipeline : skinBlurPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skinBlurLayout, 0, 1, &bindlessSet, 0, nullptr);
    SkinPushConstants push{};
    push.frame = f.frameAddress;
    push.profiles = f.frameAddress + kSkinProfilesOffset;
    push.lightTexture = mode == 1 ? skinTargets.slots[2] : skinTargets.slots[0];
    push.skinTexture = skinTargets.slots[1];
    push.depthTexture = sceneDepthSlot;
    push.mode = mode;
    push.taps = skinBlurTaps(shadingSkin.load(std::memory_order_relaxed));
    vkCmdPushConstants(cmd, skinBlurLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

}   // namespace shockwave
