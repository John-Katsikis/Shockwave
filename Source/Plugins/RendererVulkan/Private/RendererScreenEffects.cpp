// Screen effects (V1-V4; docs/Shadows.md, docs/Reflections.md, docs/PostProcess.md).
//
// With contact_shadows on, Shaders/contact_shadows.comp traces a short ray towards the sun
// from every pixel through the surface prepass's depth, between the prepass and the scene;
// mesh.glsl darkens that light by it. With reflections_quality on, Shaders/ssr.comp traces
// each surface's mirror ray at half resolution in the same place and, where it hits
// something on screen, takes that spot of last frame's scene (copied at half resolution
// after the particles, before TAA); the renderer blits four mips from it, and mesh.glsl
// mixes it over the reflection probes by confidence and roughness. After TAA, motion blur
// (Shaders/motion_blur.comp) smears the scene along its motion vectors into a second image
// that's copied back, and depth of field (dof_downsample, dof_gather, dof_composite) blurs
// what's out of a post-process profile's focus, in place.
//
// Off, an effect has no images, no descriptor sets, no pipeline and no pass: the frame is
// as it was. Images follow the settings at a frame boundary like the others, the old ones
// retired; the two read through the bindless table alternate between two slots each, like
// ambient occlusion's, so a frame in flight never sees its image change.
#include "RendererImpl.h"

#include <Renderer/ContactShadows.h>
#include <Renderer/DepthOfField.h>
#include <Renderer/MotionBlur.h>
#include <Renderer/ScreenReflections.h>

#include <initializer_list>

namespace shockwave {

namespace {

constexpr VkFormat kContactFormat = VK_FORMAT_R8_UNORM;
constexpr VkFormat kScreenColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Must match the push constants in shaders/screen_effects.glsl.
struct ScreenPushConstants {
    glm::mat4 matrix;
    VkDeviceAddress frame;
    glm::uvec2 size;
    glm::vec4 params;
    glm::vec4 params2;
    uint32_t mode;
    uint32_t steps;
    uint32_t flags;
    uint32_t pad;
};
static_assert(sizeof(ScreenPushConstants) == 128, "ScreenPushConstants must match shaders/screen_effects.glsl");

// The layout every screen-effect shader shares: four sampled images (0, 1 and 3 nearest,
// 2 linear) and two storage images. Each shader declares only the bindings it reads.
constexpr uint32_t kScreenBindings = 6;

VkExtent2D halfOf(VkExtent2D full) { return {std::max(1u, (full.width + 1) / 2), std::max(1u, (full.height + 1) / 2)}; }

struct Binding {
    uint32_t binding;
    VkImageView view;
    VkImageLayout layout;
};

VkDescriptorPool createPool(VkDevice device, uint32_t sets) {
    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * sets},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * sets},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = sets;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vk::check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
    return pool;
}

VkDescriptorSet allocateSet(
    VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout, std::initializer_list<Binding> bindings) {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    vk::check(vkAllocateDescriptorSets(device, &allocInfo, &set), "vkAllocateDescriptorSets");
    std::vector<VkDescriptorImageInfo> images;
    images.reserve(bindings.size());
    std::vector<VkWriteDescriptorSet> writes;
    for (const Binding& b : bindings) {
        images.push_back({VK_NULL_HANDLE, b.view, b.layout});
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = b.binding;
        write.descriptorCount = 1;
        write.descriptorType =
            b.binding < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes.push_back(write);
    }
    for (size_t i = 0; i < writes.size(); ++i) writes[i].pImageInfo = &images[i];
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return set;
}

// A free slot of two, other than the one in use; -1 while both are in flight.
int freeSlot(const uint32_t (&slots)[2], const bool (&busy)[2], bool have, uint32_t current) {
    for (int i = 0; i < 2; ++i)
        if (!busy[i] && !(have && slots[i] == current)) return i;
    return -1;
}

constexpr VkImageLayout kRead = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
constexpr VkImageLayout kGeneral = VK_IMAGE_LAYOUT_GENERAL;
constexpr VkAccessFlags2 kAnyRead =
    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
constexpr VkAccessFlags2 kAnyWrite = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;

// A barrier on every level of an image kept in the general layout (or from undefined).
void generalBarrier(
    VkCommandBuffer cmd, const vk::Image& image, VkImageLayout from, VkPipelineStageFlags2 srcStage,
    VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, uint32_t firstLevel = 0,
    uint32_t levels = VK_REMAINING_MIP_LEVELS) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = from;
    barrier.newLayout = kGeneral;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, firstLevel, levels, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void blit(
    VkCommandBuffer cmd, VkImage src, VkImageLayout srcLayout, uint32_t srcLevel, VkExtent2D srcExtent, VkImage dst,
    uint32_t dstLevel, VkExtent2D dstExtent) {
    VkImageBlit2 region{};
    region.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, srcLevel, 0, 1};
    region.srcOffsets[1] = {int32_t(srcExtent.width), int32_t(srcExtent.height), 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dstLevel, 0, 1};
    region.dstOffsets[1] = {int32_t(dstExtent.width), int32_t(dstExtent.height), 1};
    VkBlitImageInfo2 info{};
    info.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
    info.srcImage = src;
    info.srcImageLayout = srcLayout;
    info.dstImage = dst;
    info.dstImageLayout = kGeneral;
    info.regionCount = 1;
    info.pRegions = &region;
    info.filter = VK_FILTER_LINEAR;   // halving: a 2x2 box
    vkCmdBlitImage2(cmd, &info);
}

uint64_t screenBytes(VmaAllocator allocator, const vk::Image& image) {
    if (!image.allocation) return 0;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(allocator, image.allocation, &info);
    return info.size;
}

VkExtent2D levelExtent(VkExtent2D e, uint32_t level) {
    return {std::max(1u, e.width >> level), std::max(1u, e.height >> level)};
}

}   // namespace

void VulkanRenderer::Impl::createScreenEffects() {
    VkDevice device = context->device();
    for (uint32_t i = 0; i < 2; ++i) {
        contactSlots[i] = bindlessSlots() - 26 - i;
        reflectionSlots[i] = bindlessSlots() - 28 - i;
    }
    static_assert(kInternalTextureSlots == 29, "the screen effects' slots end the internal ones");

    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(context->physicalDevice(), kContactFormat, &props);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    contactFormatSupported = (props.optimalTilingFeatures & need) == need;

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk::check(vkCreateSampler(device, &samplerInfo, nullptr, &screenNearestSampler), "vkCreateSampler");
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    vk::check(vkCreateSampler(device, &samplerInfo, nullptr, &screenLinearSampler), "vkCreateSampler");

    VkDescriptorSetLayoutBinding bindings[kScreenBindings]{};
    for (uint32_t i = 0; i < kScreenBindings; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType =
            i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[i].pImmutableSamplers = i == 2 ? &screenLinearSampler : i < 4 ? &screenNearestSampler : nullptr;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = kScreenBindings;
    layoutInfo.pBindings = bindings;
    vk::check(
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &screenSetLayout), "vkCreateDescriptorSetLayout");

    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ScreenPushConstants)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &screenSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &range;
    vk::check(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &screenLayout), "vkCreatePipelineLayout");
}

void VulkanRenderer::Impl::destroyScreenEffects() {
    VkDevice device = context->device();
    destroyScreenEffectTargets();
    for (VkPipeline* p :
         {&contactPipeline, &ssrPipeline, &motionBlurPipeline, &dofPipelines[0], &dofPipelines[1], &dofPipelines[2]}) {
        if (*p) vkDestroyPipeline(device, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    vkDestroyPipelineLayout(device, screenLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, screenSetLayout, nullptr);
    vkDestroySampler(device, screenNearestSampler, nullptr);
    vkDestroySampler(device, screenLinearSampler, nullptr);
}

namespace {

VkPipeline createScreenPipeline(VkDevice device, VkPipelineLayout layout, const std::filesystem::path& shader) {
    VkShaderModule module = vk::loadShaderModule(device, shader);
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateComputePipelines(device, vk::pipelineCache(), 1, &info, nullptr, &pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    vk::check(result, "vkCreateComputePipelines");
    vk::notePipelineCreated();
    return pipeline;
}

}   // namespace

bool VulkanRenderer::Impl::contactWanted() const {
    return contactFormatSupported &&
           (screenEffects.contactShadows == ContactShadows::On || debugView == DebugView::ContactShadows);
}

bool VulkanRenderer::Impl::reflectionsWanted() const {
    return screenEffects.reflections != ScreenReflections::Off || debugView == DebugView::ScreenReflections;
}

bool VulkanRenderer::Impl::motionBlurWanted() const {
    return screenEffects.motionBlur > 0.0f && antiAliasing == AntiAliasing::Taa;
}

bool VulkanRenderer::Impl::dofWanted() const { return postProcessSettings.depthOfField && postProcess.dofBlur > 0.0f; }

void VulkanRenderer::Impl::destroyContactTargets(ContactTargets& targets) {
    if (targets.pool) vkDestroyDescriptorPool(context->device(), targets.pool, nullptr);
    destroyImage(targets.image);
    targets = {};
}

void VulkanRenderer::Impl::destroyReflectionTargets(ReflectionTargets& targets) {
    if (targets.pool) vkDestroyDescriptorPool(context->device(), targets.pool, nullptr);
    if (targets.resultTop) vkDestroyImageView(context->device(), targets.resultTop, nullptr);
    destroyImage(targets.previous);
    destroyImage(targets.result);
    targets = {};
}

void VulkanRenderer::Impl::destroyMotionBlurTargets(MotionBlurTargets& targets) {
    if (targets.pool) vkDestroyDescriptorPool(context->device(), targets.pool, nullptr);
    destroyImage(targets.image);
    targets = {};
}

void VulkanRenderer::Impl::destroyDofTargets(DofTargets& targets) {
    if (targets.pool) vkDestroyDescriptorPool(context->device(), targets.pool, nullptr);
    for (vk::Image& image : targets.half) destroyImage(image);
    targets = {};
}

void VulkanRenderer::Impl::destroyScreenEffectTargets() {
    destroyContactTargets(contactTargets);
    destroyReflectionTargets(reflectionTargets);
    destroyMotionBlurTargets(motionBlurTargets);
    destroyDofTargets(dofTargets);
    contactSlotBusy[0] = contactSlotBusy[1] = reflectionSlotBusy[0] = reflectionSlotBusy[1] = false;
    reflectionHistory = reflectionLayoutsReady = false;
    motionBlurState = {};
}

void VulkanRenderer::Impl::updateScreenEffectTargets() {
    VkDevice device = context->device();
    const VkExtent2D full = sceneColor.extent, half = halfOf(full);
    // Every image here reads the prepass's depth; reflections its normals too, motion blur
    // the motion vectors. Made again whenever one of those was.
    const bool prepass = prepassActive();

    // ---- Contact shadows ----
    {
        const bool want = contactWanted() && prepass;
        const bool have = contactTargets.image.image != VK_NULL_HANDLE;
        auto retireCurrent = [this] {
            if (!contactTargets.image.image) return;
            const int slot = contactTargets.slot == contactSlots[0] ? 0 : 1;
            contactSlotBusy[slot] = true;
            retire([this, old = contactTargets, slot]() mutable {
                destroyContactTargets(old);
                contactSlotBusy[slot] = false;
            });
            contactTargets = {};
        };
        if (!want && have) retireCurrent();
        if (want && !have) {
            const int free = freeSlot(contactSlots, contactSlotBusy, false, 0);
            if (free >= 0) {
                ContactTargets targets;
                targets.slot = contactSlots[free];
                targets.image = createImage(
                    full, kContactFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, 1, true);
                targets.pool = createPool(device, 1);
                targets.set = allocateSet(
                    device, targets.pool, screenSetLayout,
                    {{0, sceneDepth.view, kRead}, {4, targets.image.view, kGeneral}});
                {
                    std::lock_guard lock(descriptorMutex);
                    VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, targets.image.view, kGeneral};
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
                contactTargets = targets;
            }
        }
    }

    // ---- Screen-space reflections ----
    {
        const bool want = reflectionsWanted() && prepass;
        const bool have = reflectionTargets.result.image != VK_NULL_HANDLE;
        if (!want && have) {
            const int slot = reflectionTargets.slot == reflectionSlots[0] ? 0 : 1;
            reflectionSlotBusy[slot] = true;
            retire([this, old = reflectionTargets, slot]() mutable {
                destroyReflectionTargets(old);
                reflectionSlotBusy[slot] = false;
            });
            reflectionTargets = {};
            reflectionHistory = reflectionLayoutsReady = false;
        }
        if (want && !have) {
            const int free = freeSlot(reflectionSlots, reflectionSlotBusy, false, 0);
            if (free >= 0) {
                ReflectionTargets targets;
                targets.slot = reflectionSlots[free];
                targets.previous = createImage(
                    half, kScreenColorFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, 1, true);
                const uint32_t levels = std::min<uint32_t>(
                    kReflectionMips,
                    1 + static_cast<uint32_t>(std::floor(std::log2(std::max(half.width, half.height)))));
                targets.result = createImage(
                    half, kScreenColorFormat,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, levels, true);
                targets.levels = levels;
                VkImageViewCreateInfo viewInfo{};
                viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                viewInfo.image = targets.result.image;
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                viewInfo.format = kScreenColorFormat;
                viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vk::check(vkCreateImageView(device, &viewInfo, nullptr, &targets.resultTop), "vkCreateImageView");
                targets.pool = createPool(device, 1);
                targets.set = allocateSet(
                    device, targets.pool, screenSetLayout,
                    {{0, sceneDepth.view, kRead},
                     {1, aoTargets.normal.view, kRead},
                     {2, targets.previous.view, kGeneral},
                     {4, targets.resultTop, kGeneral}});
                {
                    std::lock_guard lock(descriptorMutex);
                    VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, targets.result.view, kGeneral};
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
                reflectionTargets = targets;
                reflectionHistory = reflectionLayoutsReady = false;
            }
        }
    }

    // ---- Motion blur ----
    {
        const bool want = motionBlurWanted() && temporalActive();
        const bool have = motionBlurTargets.image.image != VK_NULL_HANDLE;
        if (!want && have) {
            retire([this, old = motionBlurTargets]() mutable { destroyMotionBlurTargets(old); });
            motionBlurTargets = {};
        }
        if (want && !have) {
            MotionBlurTargets targets;
            targets.image = createImage(
                full, kScreenColorFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, 1, true);
            targets.pool = createPool(device, 1);
            targets.set = allocateSet(
                device, targets.pool, screenSetLayout,
                {{0, sceneColor.view, kRead},
                 {1, sceneDepth.view, kRead},
                 {3, temporalTargets.motion.view, kRead},
                 {4, targets.image.view, kGeneral}});
            motionBlurTargets = targets;
            motionBlurState = {};
        }
    }

    // ---- Depth of field ----
    {
        const bool want = dofWanted() && prepass;
        const bool have = dofTargets.half[0].image != VK_NULL_HANDLE;
        if (!want && have) {
            retire([this, old = dofTargets]() mutable { destroyDofTargets(old); });
            dofTargets = {};
        }
        if (want && !have) {
            DofTargets targets;
            for (vk::Image& image : targets.half)
                image = createImage(
                    half, kScreenColorFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, 1, true);
            targets.pool = createPool(device, 3);
            targets.sets[0] = allocateSet(
                device, targets.pool, screenSetLayout,
                {{0, sceneColor.view, kRead}, {1, sceneDepth.view, kRead}, {4, targets.half[0].view, kGeneral}});
            targets.sets[1] = allocateSet(
                device, targets.pool, screenSetLayout,
                {{3, targets.half[0].view, kGeneral}, {4, targets.half[1].view, kGeneral}});
            targets.sets[2] = allocateSet(
                device, targets.pool, screenSetLayout,
                {{1, sceneDepth.view, kRead}, {2, targets.half[1].view, kGeneral}, {5, sceneColor.view, kGeneral}});
            dofTargets = targets;
        }
    }
}

void VulkanRenderer::Impl::prepareScreenEffects(FrameData& header, const LightData* lights, uint32_t directionalCount) {
    ScreenEffectsStats stats;
    VkDevice device = context->device();
    const bool lit = debugView == DebugView::Lit;
    contactThisFrame = reflectionsThisFrame = reflectionTraceThisFrame = motionBlurThisFrame = dofThisFrame = false;

    // ---- Contact shadows: towards the first directional light that casts shadows ----
    if (contactWanted() || screenEffects.contactShadows == ContactShadows::On) {
        uint32_t sun = kNoShadow;
        for (uint32_t i = 0; i < directionalCount && sun == kNoShadow; ++i)
            if (lights[i].shadowView != kNoShadow) sun = i;
        if (!contactFormatSupported)
            stats.contactReason = "the GPU can't write 8-bit images from compute shaders";
        else if (!contactTargets.image.image || !prepassActive())
            stats.contactReason = "pending: its image is made at the next frame";
        else if (sun == kNoShadow)
            stats.contactReason = "no shadow-casting sun this frame (shadow_quality off, or no directional light)";
        else if (captureRequested)
            stats.contactReason = "reflection captures are drawn without it";
        else {
            contactThisFrame = true;
            contactSunView = glm::normalize(glm::mat3(view) * -lights[sun].direction);
            header.contactShadowTexture = contactTargets.slot;
            header.contactShadowLight = sun;
            if (!contactPipeline)
                contactPipeline = createScreenPipeline(device, screenLayout, shaderPath("contact_shadows.comp"));
        }
    }
    stats.contactShadows = contactThisFrame;
    stats.contactBytes = screenBytes(context->allocator(), contactTargets.image);

    // ---- Screen-space reflections: last frame's scene, when it's the same camera ----
    if (reflectionsWanted()) {
        if (!reflectionTargets.result.image || !prepassActive()) {
            stats.reflectionReason = "pending: its images are made at the next frame";
        } else if (captureRequested) {
            stats.reflectionReason = "reflection captures are drawn without it";
        } else {
            reflectionsThisFrame = true;
            reflectionTraceThisFrame = reflectionHistory && reflectionView == currentView && !cameraCutThisFrame;
            if (!ssrPipeline) ssrPipeline = createScreenPipeline(device, screenLayout, shaderPath("ssr.comp"));
        }
    }
    if (reflectionTraceThisFrame) {
        header.ssrTexture = reflectionTargets.slot;
        header.ssrLevels = static_cast<float>(reflectionTargets.levels);
    }
    stats.reflections = reflectionsThisFrame;
    stats.reflectionHistory = reflectionTraceThisFrame;
    stats.reflectionBytes = screenBytes(context->allocator(), reflectionTargets.previous) +
                            screenBytes(context->allocator(), reflectionTargets.result);

    // ---- Motion blur: TAA's motion vectors, so only on frames TAA resolves ----
    if (screenEffects.motionBlur > 0.0f) {
        if (antiAliasing != AntiAliasing::Taa)
            stats.motionBlurReason = "motion blur needs anti_aliasing taa for its motion vectors";
        else if (!motionBlurTargets.image.image)
            stats.motionBlurReason = "pending: its image is made at the next frame";
        else if (!lastTemporalStats.taa)
            stats.motionBlurReason = "no TAA this frame (a debug view, a capture or a camera without a view)";
        else if (lastTemporalStats.reset != HistoryReset::None) {
            // A frame without motion (first frame, cut or resize) isn't a fallback.
        } else {
            const float dt = postProcess.deltaTime > 0.0f ? postProcess.deltaTime : 1.0f / 60.0f;
            motionBlurScale = std::clamp(screenEffects.motionBlur, 0.0f, 1.0f) * kMotionBlurShutter / dt;
            motionBlurThisFrame = true;
            if (!motionBlurPipeline)
                motionBlurPipeline = createScreenPipeline(device, screenLayout, shaderPath("motion_blur.comp"));
        }
    }
    stats.motionBlur = motionBlurThisFrame;
    stats.motionBlurBytes = screenBytes(context->allocator(), motionBlurTargets.image);

    // ---- Depth of field: the post-process profile's focus ----
    if (postProcess.dofBlur > 0.0f) {
        dofRadius = dofRadiusPixels(postProcess.dofBlur, sceneColor.extent.height);
        if (!postProcessSettings.depthOfField)
            stats.depthOfFieldReason = "";   // turned off by the player
        else if (!dofTargets.half[0].image || !prepassActive())
            stats.depthOfFieldReason = "pending: its images are made at the next frame";
        else if (!lit)
            stats.depthOfFieldReason = "debug views are drawn without it";
        else if (captureRequested)
            stats.depthOfFieldReason = "reflection captures are drawn without it";
        else if (dofRadius < 0.5f)
            stats.depthOfFieldReason = "dof_blur is under half a pixel at this resolution";
        else {
            dofThisFrame = true;
            const char* shaders[3] = {"dof_downsample.comp", "dof_gather.comp", "dof_composite.comp"};
            for (int i = 0; i < 3; ++i)
                if (!dofPipelines[i])
                    dofPipelines[i] = createScreenPipeline(device, screenLayout, shaderPath(shaders[i]));
        }
    }
    stats.depthOfField = dofThisFrame;
    stats.depthOfFieldBytes =
        screenBytes(context->allocator(), dofTargets.half[0]) + screenBytes(context->allocator(), dofTargets.half[1]);
    lastScreenEffectsStats = stats;
}

void VulkanRenderer::Impl::finishScreenEffects() {
    reflectionHistory = reflectionsThisFrame;
    reflectionView = currentView;
    reflectionPreviousViewProjection = viewProjection;
}

void VulkanRenderer::Impl::recordContactShadows(VkCommandBuffer cmd, Frame& f) {
    // Last frame's contents aren't needed; wait for last frame's scene to finish reading.
    generalBarrier(
        cmd, contactTargets.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, contactPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, screenLayout, 0, 1, &contactTargets.set, 0, nullptr);
    const VkExtent2D full = sceneColor.extent;
    ScreenPushConstants push{};
    push.frame = f.frameAddress;
    push.size = {full.width, full.height};
    push.params = glm::vec4(contactSunView, kContactShadowLength);
    push.params2 = glm::vec4(kContactShadowThickness, 0.0f, 0.0f, 0.0f);
    push.steps = kContactShadowSteps;
    vkCmdPushConstants(cmd, screenLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (full.width + 7) / 8, (full.height + 7) / 8, 1);
    generalBarrier(
        cmd, contactTargets.image, kGeneral, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void VulkanRenderer::Impl::recordReflections(VkCommandBuffer cmd, Frame& f) {
    const vk::Image& result = reflectionTargets.result;
    const VkExtent2D half = result.extent;
    // Last frame's result isn't needed; wait for last frame's scene to finish reading it.
    generalBarrier(
        cmd, result, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ssrPipeline);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, screenLayout, 0, 1, &reflectionTargets.set, 0, nullptr);
    const uint32_t steps = screenReflectionSteps(
        screenEffects.reflections == ScreenReflections::Off ? ScreenReflections::Medium : screenEffects.reflections);
    ScreenPushConstants push{};
    push.matrix = reflectionPreviousViewProjection * glm::inverse(view);
    push.frame = f.frameAddress;
    push.size = {half.width, half.height};
    push.params = glm::vec4(
        kReflectionMaxDistance, kReflectionThickness, reflectionFirstStep(steps, kReflectionMaxDistance),
        kReflectionStepGrowth);
    push.steps = steps;
    push.flags = 1u;   // last frame's scene is there (the pass only runs when it is)
    vkCmdPushConstants(cmd, screenLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (half.width + 7) / 8, (half.height + 7) / 8, 1);
    // The mips for rough surfaces: each a 2x2 box of the one above.
    for (uint32_t level = 1; level < reflectionTargets.levels; ++level) {
        generalBarrier(
            cmd, result, kGeneral, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            kAnyWrite, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, level - 1, 1);
        blit(
            cmd, result.image, kGeneral, level - 1, levelExtent(half, level - 1), result.image, level,
            levelExtent(half, level));
    }
    generalBarrier(
        cmd, result, kGeneral, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, kAnyWrite,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void VulkanRenderer::Impl::recordReflectionCopy(VkCommandBuffer cmd) {
    const vk::Image& previous = reflectionTargets.previous;
    if (!reflectionLayoutsReady) {
        // New images: into the general layout they stay in from here on.
        generalBarrier(
            cmd, reflectionTargets.result, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE);
        generalBarrier(
            cmd, previous, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        reflectionLayoutsReady = true;
    } else {
        // This frame's trace has read last frame's copy.
        generalBarrier(
            cmd, previous, kGeneral, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    blit(
        cmd, sceneColor.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, sceneColor.extent, previous.image, 0,
        previous.extent);
    generalBarrier(
        cmd, previous, kGeneral, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void VulkanRenderer::Impl::recordMotionBlur(VkCommandBuffer cmd, Frame& f) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, motionBlurPipeline);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, screenLayout, 0, 1, &motionBlurTargets.set, 0, nullptr);
    const VkExtent2D full = sceneColor.extent;
    ScreenPushConstants push{};
    push.frame = f.frameAddress;
    push.size = {full.width, full.height};
    push.params = glm::vec4(motionBlurScale, kMotionBlurMaxPixels, kMotionBlurMinPixels, 0.0f);
    push.steps = kMotionBlurSamples;
    vkCmdPushConstants(cmd, screenLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (full.width + 7) / 8, (full.height + 7) / 8, 1);
}

void VulkanRenderer::Impl::recordMotionBlurCopy(VkCommandBuffer cmd) {
    VkImageCopy2 region{};
    region.sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2;
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {sceneColor.extent.width, sceneColor.extent.height, 1};
    VkCopyImageInfo2 copy{};
    copy.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2;
    copy.srcImage = motionBlurTargets.image.image;
    copy.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    copy.dstImage = sceneColor.image;
    copy.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    copy.regionCount = 1;
    copy.pRegions = &region;
    vkCmdCopyImage2(cmd, &copy);
}

void VulkanRenderer::Impl::recordDepthOfField(VkCommandBuffer cmd, Frame& f, uint32_t step) {
    const VkExtent2D full = sceneColor.extent, half = dofTargets.half[0].extent;
    if (step == 0) {
        // Last frame's contents aren't needed; wait for last frame's passes to finish with them.
        for (const vk::Image& image : dofTargets.half)
            generalBarrier(
                cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dofPipelines[step]);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, screenLayout, 0, 1, &dofTargets.sets[step], 0, nullptr);
    const VkExtent2D size = step == 2 ? full : half;
    ScreenPushConstants push{};
    push.frame = f.frameAddress;
    push.size = {size.width, size.height};
    push.params = glm::vec4(
        postProcess.dofFocusDistance, postProcess.dofFocusRange, dofRadius, dofFalloff(postProcess.dofFocusDistance));
    push.mode = step;
    push.steps = kDofGatherSamples;
    vkCmdPushConstants(cmd, screenLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (size.width + 7) / 8, (size.height + 7) / 8, 1);
    if (step < 2) {
        // The next step reads what this one wrote.
        generalBarrier(
            cmd, dofTargets.half[step], kGeneral, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
}

}   // namespace shockwave
