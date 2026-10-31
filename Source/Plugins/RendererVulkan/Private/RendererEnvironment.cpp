// Image-based lighting. The scene's surroundings, an environment map or the Environment's
// sky and ground colors, light every surface from all directions:
//   1. environment_sky.comp draws them into a cube map, whose mip chain is then blitted;
//   2. environment_filter.comp convolves that cube into a small irradiance cube (diffuse
//      light) and a prefiltered specular cube, one roughness per mip level (reflections);
//   3. at startup it also fills the BRDF table, the other half of the split-sum
//      approximation (Karis, "Real Shading in Unreal Engine 4").
// mesh.frag reads the three. The cubes are only rebuilt when what they're made from
// changes, inside that frame's command buffer, so nothing waits for the GPU. sky.frag
// draws an environment map behind the scene.
#include "RendererImpl.h"

namespace shockwave {

namespace {

constexpr VkFormat kCubeFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr uint32_t kEnvironmentSize = 256;   // per face; the source the filters read
constexpr uint32_t kSpecularSize = 128;
constexpr uint32_t kSpecularLevels = 6;   // 128 .. 4: roughness 0, 0.2 .. 1
constexpr uint32_t kIrradianceSize = 32;
constexpr uint32_t kBrdfTableSize = 128;
constexpr uint32_t kSpecularSamples = 96;
constexpr uint32_t kIrradianceSamples = 256;
constexpr uint32_t kBrdfSamples = 512;

// Must match the push constants in shaders/environment_sky.comp.
struct EnvironmentSkyPushConstants {
    glm::vec3 skyColor;
    uint32_t map;   // bindless slot of the environment map; 0: the sky and ground colors
    glm::vec3 groundColor;
    float intensity;
    glm::vec2 rotation;   // cosine and sine of the map's turn about +Y
    float mapLevel;   // the map's mip level whose texels are about a cube texel wide
    uint32_t size;
};
static_assert(sizeof(EnvironmentSkyPushConstants) == 48, "must match environment_sky.comp");

// Must match the push constants in shaders/environment_filter.comp.
struct EnvironmentFilterPushConstants {
    float roughness;
    uint32_t size;   // of the level being written
    uint32_t samples;
    uint32_t mode;   // 0 specular, 1 irradiance, 2 BRDF table
    float sourceSize;   // the environment cube's top level
    float sourceLevels;
    float pad[2];
};
static_assert(sizeof(EnvironmentFilterPushConstants) == 32, "must match environment_filter.comp");

// Must match the push constants in shaders/sky.vert.
struct SkyPushConstants {
    glm::mat4 inverseViewProjection;   // of the camera's rotation alone
    VkDeviceAddress frame;   // the frame's FrameData, for fog (A06)
    glm::vec4 background;   // w 1: no environment map, this colour instead
};
static_assert(sizeof(SkyPushConstants) == 88, "must match sky.vert and sky.frag");

// The cubes are rebuilt rarely, so their barriers simply order everything before and after.
void cubeBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    transitionImage(
        cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, from, to, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, 0, VK_REMAINING_MIP_LEVELS,
        VK_REMAINING_ARRAY_LAYERS);
}

uint32_t groups(uint32_t size) { return (size + 7) / 8; }

}   // namespace

VulkanRenderer::Impl::CubeMap VulkanRenderer::Impl::createCubeMap(uint32_t size, uint32_t levels) {
    VkDevice device = context->device();
    CubeMap cube;
    cube.size = size;
    cube.levels = levels;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kCubeFormat;
    imageInfo.extent = {size, size, 1};
    imageInfo.mipLevels = levels;
    imageInfo.arrayLayers = 6;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    vk::check(
        vmaCreateImage(context->allocator(), &imageInfo, &allocInfo, &cube.image, &cube.allocation, nullptr),
        "vmaCreateImage");

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = cube.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = kCubeFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 6};
    vk::check(vkCreateImageView(device, &viewInfo, nullptr, &cube.view), "vkCreateImageView");

    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    for (uint32_t level = 0; level < levels; ++level) {
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 6};
        VkImageView view = VK_NULL_HANDLE;
        vk::check(vkCreateImageView(device, &viewInfo, nullptr, &view), "vkCreateImageView");
        cube.levelViews.push_back(view);
    }
    return cube;
}

void VulkanRenderer::Impl::destroyCubeMap(CubeMap& cube) {
    VkDevice device = context->device();
    for (VkImageView view : cube.levelViews) vkDestroyImageView(device, view, nullptr);
    if (cube.view) vkDestroyImageView(device, cube.view, nullptr);
    if (cube.image) vmaDestroyImage(context->allocator(), cube.image, cube.allocation);
    cube = {};
}

void VulkanRenderer::Impl::createEnvironment() {
    VkDevice device = context->device();
    environmentCube = createCubeMap(kEnvironmentSize, static_cast<uint32_t>(std::log2(kEnvironmentSize)) + 1);
    specularCube = createCubeMap(kSpecularSize, kSpecularLevels);
    irradianceCube = createCubeMap(kIrradianceSize, 1);
    brdfTable = createImage(
        {kBrdfTableSize, kBrdfTableSize}, kCubeFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = brdfTable.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = kCubeFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk::check(vkCreateImageView(device, &viewInfo, nullptr, &brdfTableTarget), "vkCreateImageView");

    // Storage image sets: the environment's top level, every specular level, the
    // irradiance cube and the BRDF table. Every shader writes an image2DArray.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    vk::check(
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &environmentTargetLayout),
        "vkCreateDescriptorSetLayout");
    const uint32_t targetCount = 1 + kSpecularLevels + 1 + 1;
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, targetCount};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = targetCount;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    vk::check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &environmentPool), "vkCreateDescriptorPool");
    auto targetSet = [&](VkImageView view) {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = environmentPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &environmentTargetLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        vk::check(vkAllocateDescriptorSets(device, &allocInfo, &set), "vkAllocateDescriptorSets");
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageView = view;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        return set;
    };
    environmentCube.levelTargets.push_back(targetSet(environmentCube.levelViews[0]));   // the rest are blitted
    for (VkImageView view : specularCube.levelViews) specularCube.levelTargets.push_back(targetSet(view));
    irradianceCube.levelTargets.push_back(targetSet(irradianceCube.levelViews[0]));
    brdfTableSet = targetSet(brdfTableTarget);

    // The cubes and the BRDF table in the bindless set, where every shader can sample them.
    {
        std::lock_guard lock(descriptorMutex);
        const VkDescriptorImageInfo images[4] = {
            {VK_NULL_HANDLE, environmentCube.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, specularCube.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, irradianceCube.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, brdfTable.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        };
        VkWriteDescriptorSet writes[4]{};
        for (uint32_t i = 0; i < 4; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = bindlessSet;
            writes[i].dstBinding = i < 3 ? 3 + i : 0;
            writes[i].dstArrayElement = i < 3 ? 0 : brdfSlot;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
    }

    // Both compute shaders read through the bindless set and write a storage image.
    VkPushConstantRange computeRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(EnvironmentSkyPushConstants)};
    static_assert(sizeof(EnvironmentSkyPushConstants) >= sizeof(EnvironmentFilterPushConstants));
    const VkDescriptorSetLayout setLayouts[2] = {bindlessLayout, environmentTargetLayout};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 2;
    pipelineLayoutInfo.pSetLayouts = setLayouts;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &computeRange;
    vk::check(
        vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &environmentLayout), "vkCreatePipelineLayout");
    auto computePipeline = [&](const char* shader) {
        VkShaderModule module = vk::loadShaderModule(device, shaderPath(shader));
        VkComputePipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";
        info.layout = environmentLayout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        vk::check(result, "vkCreateComputePipelines");
        return pipeline;
    };
    environmentSkyPipeline = computePipeline("environment_sky.comp");
    environmentFilterPipeline = computePipeline("environment_filter.comp");

    // The sky: one fullscreen triangle at the far plane, under everything the scene draws.
    VkPushConstantRange skyRange{
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SkyPushConstants)};
    VkPipelineLayoutCreateInfo skyLayoutInfo{};
    skyLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    skyLayoutInfo.setLayoutCount = 1;
    skyLayoutInfo.pSetLayouts = &bindlessLayout;
    skyLayoutInfo.pushConstantRangeCount = 1;
    skyLayoutInfo.pPushConstantRanges = &skyRange;
    vk::check(vkCreatePipelineLayout(device, &skyLayoutInfo, nullptr, &skyLayout), "vkCreatePipelineLayout");
    vk::GraphicsPipelineDesc skyDesc;
    skyDesc.vertexShader = shaderPath("sky.vert");
    skyDesc.fragmentShader = shaderPath("sky.frag");
    skyDesc.colorFormat = kSceneColorFormat;
    skyDesc.depthFormat = kDepthFormat;
    skyDesc.layout = skyLayout;
    skyDesc.cullMode = VK_CULL_MODE_NONE;
    skyDesc.depthWrite = false;   // at depth 0, which passes against the cleared depth
    skyPipeline = vk::createGraphicsPipeline(device, skyDesc);

    // Everything starts out sampleable; the BRDF table is made now, once. The cubes are
    // made by the first frame (environmentChanged).
    immediateSubmit([&](VkCommandBuffer cmd) {
        for (VkImage image : {environmentCube.image, specularCube.image, irradianceCube.image})
            cubeBarrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        cubeBarrier(cmd, brdfTable.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, environmentFilterPipeline);
        const VkDescriptorSet sets[2] = {bindlessSet, brdfTableSet};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, environmentLayout, 0, 2, sets, 0, nullptr);
        EnvironmentFilterPushConstants push{};
        push.size = kBrdfTableSize;
        push.samples = kBrdfSamples;
        push.mode = 2;
        vkCmdPushConstants(cmd, environmentLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, groups(kBrdfTableSize), groups(kBrdfTableSize), 1);
        cubeBarrier(cmd, brdfTable.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    environmentBuilt = false;
}

void VulkanRenderer::Impl::destroyEnvironment() {
    VkDevice device = context->device();
    vkDestroyPipeline(device, skyPipeline, nullptr);
    vkDestroyPipelineLayout(device, skyLayout, nullptr);
    vkDestroyPipeline(device, environmentSkyPipeline, nullptr);
    vkDestroyPipeline(device, environmentFilterPipeline, nullptr);
    vkDestroyPipelineLayout(device, environmentLayout, nullptr);
    vkDestroyDescriptorPool(device, environmentPool, nullptr);
    vkDestroyDescriptorSetLayout(device, environmentTargetLayout, nullptr);
    if (brdfTableTarget) vkDestroyImageView(device, brdfTableTarget, nullptr);
    brdfTableTarget = VK_NULL_HANDLE;
    destroyImage(brdfTable);
    destroyCubeMap(environmentCube);
    destroyCubeMap(specularCube);
    destroyCubeMap(irradianceCube);
}

VkImage VulkanRenderer::Impl::environmentMapImage() const {
    const uint32_t id = lighting.environmentMap.id;
    return id != 0 && isTexture(id) ? textures[id].image : VK_NULL_HANDLE;
}

bool VulkanRenderer::Impl::environmentChanged() {
    EnvironmentKey key;
    key.map = environmentMapImage();
    if (key.map) {
        key.intensity = lighting.environmentIntensity;
        key.rotation = lighting.environmentRotation;
    } else {
        key.sky = lighting.skyAmbient;
        key.ground = lighting.groundAmbient;
    }
    if (environmentBuilt && key == environmentKey) return false;
    environmentKey = key;
    environmentBuilt = true;
    return true;
}

void VulkanRenderer::Impl::recordEnvironment(VkCommandBuffer cmd) {
    const VkDescriptorSet bindless = bindlessSet;
    auto bindTarget = [&](VkDescriptorSet target) {
        const VkDescriptorSet sets[2] = {bindless, target};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, environmentLayout, 0, 2, sets, 0, nullptr);
    };

    // 1. The surroundings into the environment cube's top level.
    cubeBarrier(cmd, environmentCube.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, environmentSkyPipeline);
    bindTarget(environmentCube.levelTargets[0]);
    EnvironmentSkyPushConstants sky{};
    sky.skyColor = environmentKey.sky;
    sky.groundColor = environmentKey.ground;
    sky.intensity = environmentKey.intensity;
    sky.rotation = {std::cos(environmentKey.rotation), std::sin(environmentKey.rotation)};
    sky.size = environmentCube.size;
    if (environmentKey.map) {
        sky.map = textureSlot(lighting.environmentMap.id);
        // The map's width spans four cube faces.
        const float width = static_cast<float>(textures[lighting.environmentMap.id].extent.width);
        sky.mapLevel = std::max(0.0f, std::log2(width / (4.0f * static_cast<float>(environmentCube.size))));
    }
    vkCmdPushConstants(cmd, environmentLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sky), &sky);
    vkCmdDispatch(cmd, groups(environmentCube.size), groups(environmentCube.size), 6);

    // 2. Its mip chain, which the filters read to keep few samples smooth.
    for (uint32_t level = 1; level < environmentCube.levels; ++level) {
        cubeBarrier(cmd, environmentCube.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        const auto from = static_cast<int32_t>(environmentCube.size >> (level - 1));
        const auto to = std::max(1, from / 2);
        VkImageBlit2 region{};
        region.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 6};
        region.srcOffsets[1] = {from, from, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 6};
        region.dstOffsets[1] = {to, to, 1};
        VkBlitImageInfo2 blit{};
        blit.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
        blit.srcImage = environmentCube.image;
        blit.srcImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        blit.dstImage = environmentCube.image;
        blit.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        blit.regionCount = 1;
        blit.pRegions = &region;
        blit.filter = VK_FILTER_LINEAR;
        vkCmdBlitImage2(cmd, &blit);
    }
    cubeBarrier(cmd, environmentCube.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // 3. Filtered into reflections, one roughness per level, and diffuse light.
    cubeBarrier(cmd, specularCube.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    cubeBarrier(cmd, irradianceCube.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, environmentFilterPipeline);
    EnvironmentFilterPushConstants filter{};
    filter.sourceSize = static_cast<float>(environmentCube.size);
    filter.sourceLevels = static_cast<float>(environmentCube.levels);
    for (uint32_t level = 0; level < specularCube.levels; ++level) {
        bindTarget(specularCube.levelTargets[level]);
        filter.roughness = static_cast<float>(level) / static_cast<float>(specularCube.levels - 1);
        filter.size = std::max(1u, specularCube.size >> level);
        filter.samples = kSpecularSamples;
        filter.mode = 0;
        vkCmdPushConstants(cmd, environmentLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(filter), &filter);
        vkCmdDispatch(cmd, groups(filter.size), groups(filter.size), 6);
    }
    bindTarget(irradianceCube.levelTargets[0]);
    filter.roughness = 1.0f;
    filter.size = irradianceCube.size;
    filter.samples = kIrradianceSamples;
    filter.mode = 1;
    vkCmdPushConstants(cmd, environmentLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(filter), &filter);
    vkCmdDispatch(cmd, groups(filter.size), groups(filter.size), 6);
    cubeBarrier(cmd, specularCube.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cubeBarrier(cmd, irradianceCube.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

bool VulkanRenderer::Impl::showsSky() const { return lighting.showEnvironment && environmentKey.map; }

void VulkanRenderer::Impl::recordSky(VkCommandBuffer cmd, VkDeviceAddress frame) {
    SkyPushConstants push;
    push.inverseViewProjection = glm::inverse(projection * glm::mat4(glm::mat3(view)));
    push.frame = frame;
    push.background = showsSky() ? glm::vec4(0.0f) : glm::vec4(clearColor.r, clearColor.g, clearColor.b, 1.0f);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyLayout, 0, 1, &bindlessSet, 0, nullptr);
    vkCmdPushConstants(
        cmd, skyLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

}   // namespace shockwave
