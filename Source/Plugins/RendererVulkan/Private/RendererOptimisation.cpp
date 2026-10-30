// Optimised assets (docs/Optimisation.md): levels of detail picked per draw, and textures
// uploaded in the GPU's compressed formats with the mipmaps they came with.
#include "RendererImpl.h"

namespace shockwave {

namespace {
// LodSettings::showLevels: one colour per level, full detail first.
constexpr std::array<glm::vec3, kMaxMeshLods + 1> kLevelColors = {{
    {0.35f, 0.85f, 0.40f},   // green
    {0.95f, 0.85f, 0.25f},   // yellow
    {1.00f, 0.55f, 0.20f},   // orange
    {0.95f, 0.30f, 0.30f},   // red
    {0.90f, 0.35f, 0.85f},   // magenta
    {0.45f, 0.45f, 1.00f},   // blue
    {0.30f, 0.85f, 0.95f},   // cyan
    {0.60f, 0.40f, 0.90f},   // purple
}};

}   // namespace

VkFormat detail::vulkanFormat(TextureFormat format, bool srgb) {
    switch (format) {
    case TextureFormat::RGBA8: return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::BC7: return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    case TextureFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

void VulkanRenderer::Impl::setMeshLods(
    MeshHandle handle, std::span<const MeshLod> lods, std::span<const uint32_t> lodIndices) {
    if (handle.id >= kMaxMeshes || !meshReady[handle.id].load()) throw std::runtime_error("setMeshLods: no such mesh");
    if (lods.size() > kMaxMeshLods) throw std::runtime_error("setMeshLods: too many levels");
    Mesh& mesh = meshes[handle.id];
    if (mesh.lodIndices.buffer) throw std::runtime_error("setMeshLods: the mesh already has levels");
    // Called right after createMesh, before anything draws the mesh.
    mesh.lodIndices = uploadBuffer(lodIndices.data(), lodIndices.size_bytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    std::copy(lods.begin(), lods.end(), mesh.lods.begin());
    mesh.lodCount = static_cast<uint32_t>(lods.size());
}

// A level's error is in the mesh's own units. Scaled into the world and projected at the
// draw's distance, it must stay under maxErrorPixels; the simplest level that does wins.
void VulkanRenderer::Impl::pickLod(const Mesh& mesh, float scale, const glm::vec4& bounds, DrawCommand& command) const {
    uint32_t level = 0;   // the full mesh; 1 and up are mesh.lods[level - 1]
    // Orthographic projections (projection[2][3] == 0) have no distance to go by.
    if (lodSettings.maxErrorPixels > 0 && projection[2][3] != 0 && scale > 0) {
        const float pixelsPerUnit = std::abs(projection[1][1]) * 0.5f * static_cast<float>(sceneColor.extent.height);
        // The nearest point of the bounding sphere, so a mesh never coarsens up close.
        const float distance = glm::length(glm::vec3(bounds) - cameraPosition) - bounds.w;
        if (distance > 0 && pixelsPerUnit > 0) {
            const float allowed = lodSettings.maxErrorPixels * distance / (pixelsPerUnit * scale);
            for (uint32_t i = mesh.lodCount; i > 0; --i) {
                if (mesh.lods[i - 1].error <= allowed) {
                    level = i;
                    break;
                }
            }
        }
    }
    if (level > 0) {
        const MeshLod& lod = mesh.lods[level - 1];
        command.indexBuffer = mesh.lodIndices.buffer;
        command.firstIndex = lod.firstIndex;
        command.indexCount = lod.indexCount;
        command.fullIndexCount = mesh.indexCount;
    }
    if (lodSettings.showLevels) command.data.tint *= glm::vec4(kLevelColors[level], 1.0f);
}

bool VulkanRenderer::Impl::formatSampleable(VkFormat format) const {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(context->physicalDevice(), format, &properties);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    return (properties.optimalTilingFeatures & need) == need;
}

bool VulkanRenderer::Impl::supportsTextureFormat(TextureFormat format) const {
    switch (format) {
    case TextureFormat::RGBA8: return true;
    case TextureFormat::BC7:
        return context->textureCompressionBC() && formatSampleable(VK_FORMAT_BC7_SRGB_BLOCK) &&
               formatSampleable(VK_FORMAT_BC7_UNORM_BLOCK);
    case TextureFormat::RGBA16F: {
        // Its mipmaps are made with blits, like RGBA8's.
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(context->physicalDevice(), VK_FORMAT_R16G16B16A16_SFLOAT, &properties);
        const VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        return formatSampleable(VK_FORMAT_R16G16B16A16_SFLOAT) && (properties.optimalTilingFeatures & blit) == blit;
    }
    }
    return false;
}

void VulkanRenderer::Impl::createTextureData(TextureHandle handle, const TextureData& data) {
    const uint32_t id = handle.id;
    if (id >= textureCapacity() || textures[id].image) throw std::runtime_error("createTexture: texture id is taken");
    if (data.shape == TextureShape::Cube) {
        vk::Image image = uploadCube(data);
        publishCube(id, image);
        return;
    }
    vk::Image image = uploadTextureData(data);
    publishTexture(id, image);
}

void VulkanRenderer::Impl::updateTextureData(TextureHandle handle, const TextureData& data) {
    if (handle.id == 0 || handle.id >= textureCapacity())
        throw std::runtime_error("updateTexture: not a texture made with createTexture");
    if (data.shape != TextureShape::Texture2D || cubeSlot(handle.id) != kNoSlot)
        throw std::runtime_error("updateTexture: cube textures are replaced by making a new one");
    vk::Image image = uploadTextureData(data);
    std::lock_guard lock(textureSwapMutex);
    textureSwaps.push_back({handle.id, image});
}

vk::Image VulkanRenderer::Impl::uploadTextureData(const TextureData& data) {
    if (!data.valid()) throw std::runtime_error("texture data is incomplete");
    if (!supportsTextureFormat(data.format))
        throw std::runtime_error(std::string("this GPU can't sample ") + textureFormatName(data.format));
    if (data.format != TextureFormat::BC7 && data.levels.size() == 1) {
        const auto& level = data.levels[0];
        return uploadTexture(
            level.width, level.height, {data.bytes.data() + level.offset, level.size},
            vulkanFormat(data.format, data.srgb));
    }
    const auto levels = static_cast<uint32_t>(data.levels.size());
    vk::Image image = createImage(
        {data.width, data.height}, vulkanFormat(data.format, data.srgb),
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, levels);
    std::vector<VkBufferImageCopy> copies(levels);
    for (uint32_t i = 0; i < levels; ++i) {
        copies[i].bufferOffset = data.levels[i].offset;
        copies[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        copies[i].imageExtent = {data.levels[i].width, data.levels[i].height, 1};
    }
    vk::Buffer staging = createStaging(data.bytes.data(), data.bytes.size());
    immediateSubmit([&](VkCommandBuffer cmd) {
        transitionImage(
            cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdCopyBufferToImage(
            cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, copies.data());
        transitionImage(
            cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    vmaDestroyBuffer(context->allocator(), staging.buffer, staging.allocation);
    return image;
}

}   // namespace shockwave
