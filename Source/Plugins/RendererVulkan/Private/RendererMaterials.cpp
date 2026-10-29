// Material samplers (G02). Each distinct SamplerDesc the Renderer hands a handle for gets a
// VkSampler in binding 6 of the bindless set, at its handle's index in the current bank.
// Draws carry the slot, never the desc, so sampling costs nothing extra per draw.
//
// Changing the anisotropy setting remakes every sampler in the other bank, switches the
// bank at a frame boundary, and retires the old samplers once the frames using them finish:
// frames in flight keep reading descriptors nobody writes, and nothing waits for the GPU.
#include "RendererImpl.h"

namespace shockwave {

namespace {
VkSamplerAddressMode addressMode(TextureWrap wrap) {
    switch (wrap) {
    case TextureWrap::Clamp: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case TextureWrap::Mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case TextureWrap::Repeat: break;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}
VkFilter filter(TextureFilter f) { return f == TextureFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR; }
}   // namespace

float VulkanRenderer::Impl::effectiveAnisotropy(const SamplerDesc& desc) const {
    // Anisotropic filtering only makes sense with linear minification and mipmaps; nearest
    // (pixel art) stays nearest whatever the setting.
    if (desc.minFilter == TextureFilter::Nearest || !desc.mipmaps) return 1.0f;
    return std::min({float(anisotropy), float(desc.maxAnisotropy), context->maxAnisotropy()});
}

VkSampler VulkanRenderer::Impl::makeSampler(const SamplerDesc& desc) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = filter(desc.magFilter);
    info.minFilter = filter(desc.minFilter);
    info.mipmapMode =
        desc.mipFilter == TextureFilter::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = addressMode(desc.wrapU);
    info.addressModeV = addressMode(desc.wrapV);
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    const float aniso = effectiveAnisotropy(desc);
    info.anisotropyEnable = aniso > 1.0f ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = aniso;
    // Without mipmaps, glTF samples the largest level: here, the largest resident one.
    info.maxLod = desc.mipmaps ? VK_LOD_CLAMP_NONE : 0.0f;
    VkSampler sampler = VK_NULL_HANDLE;
    vk::check(vkCreateSampler(context->device(), &info, nullptr, &sampler), "vkCreateSampler");
    return sampler;
}

void VulkanRenderer::Impl::writeSamplerDescriptor(uint32_t slot, VkSampler sampler) {
    std::lock_guard lock(descriptorMutex);
    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = sampler;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = bindlessSet;
    write.dstBinding = 6;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(context->device(), 1, &write, 0, nullptr);
}

// Any thread. A new handle's slot isn't read by any frame yet, so writing it is safe.
void VulkanRenderer::Impl::createSampler(SamplerHandle handle, const SamplerDesc& desc) {
    if (handle.id == 0 || handle.id >= kMaxSamplers) return;
    std::lock_guard lock(samplerMutex);
    if (samplerDescs.size() <= handle.id) samplerDescs.resize(handle.id + 1);
    samplerDescs[handle.id] = desc;
    VkSampler& sampler = samplerObjects[samplerBank][handle.id];
    if (sampler) vkDestroySampler(context->device(), sampler, nullptr);   // a replayed id
    sampler = makeSampler(desc);
    writeSamplerDescriptor(samplerBank * kMaxSamplers + handle.id, sampler);
}

void VulkanRenderer::Impl::setAnisotropy(uint32_t value) {
    value = std::clamp(value, 1u, 16u);
    if (value == anisotropy) return;
    anisotropy = value;
    needsSamplerRebuild = true;
}

void VulkanRenderer::Impl::rebuildSamplers() {
    if (!needsSamplerRebuild) return;
    const uint32_t next = (samplerBank + 1) % kSamplerBanks;
    if (samplerBankBusy[next]) return;   // its old samplers are still in flight; next frame
    std::lock_guard lock(samplerMutex);
    for (uint32_t id = 0; id < samplerDescs.size(); ++id) {
        samplerObjects[next][id] = makeSampler(samplerDescs[id]);
        writeSamplerDescriptor(next * kMaxSamplers + id, samplerObjects[next][id]);
    }
    const uint32_t old = samplerBank;
    samplerBankBusy[old] = true;
    retire([this, old, objects = samplerObjects[old]] {
        for (VkSampler sampler : objects)
            if (sampler) vkDestroySampler(context->device(), sampler, nullptr);
        samplerBankBusy[old] = false;
    });
    samplerObjects[old] = {};
    samplerBank = next;
    needsSamplerRebuild = false;
}

void VulkanRenderer::Impl::destroySamplers() {
    for (auto& bank : samplerObjects)
        for (VkSampler& sampler : bank) {
            if (sampler) vkDestroySampler(context->device(), sampler, nullptr);
            sampler = VK_NULL_HANDLE;
        }
}

}   // namespace shockwave
