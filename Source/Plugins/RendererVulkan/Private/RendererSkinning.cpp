#include "RendererImpl.h"

#include <Core/Profile.h>

namespace shockwave {

// GPU skinning. drawSkinnedMesh records the draw like any other, plus its joint matrices.
// At the end of the frame every skinned draw gets its own stretch of a per-frame vertex
// buffer, a compute pass writes the posed vertices there, and the draw's vertex pointer
// is aimed at them. The scene and every shadow map then read posed vertices with no
// skinning code of their own, and a character is skinned once however many shadow maps
// it's in.

void VulkanRenderer::Impl::createSkinningPipeline() {
    VkDevice device = context->device();
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinningPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    vk::check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &skinningLayout), "vkCreatePipelineLayout");

    VkShaderModule module = vk::loadShaderModule(device, shaderPath("skinning.comp"));
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = skinningLayout;
    const VkResult result = vkCreateComputePipelines(device, vk::pipelineCache(), 1, &info, nullptr, &skinningPipeline);
    vkDestroyShaderModule(device, module, nullptr);
    vk::check(result, "vkCreateComputePipelines");
}

void VulkanRenderer::Impl::destroySkinning() {
    VkDevice device = context->device();
    VmaAllocator allocator = context->allocator();
    for (Frame& f : frames) {
        if (f.jointBuffer.buffer) vmaDestroyBuffer(allocator, f.jointBuffer.buffer, f.jointBuffer.allocation);
        if (f.skinnedBuffer.buffer) vmaDestroyBuffer(allocator, f.skinnedBuffer.buffer, f.skinnedBuffer.allocation);
        f.jointBuffer = {};
        f.skinnedBuffer = {};
    }
    for (uint32_t i = 0; i < kMaxMeshes; ++i) {
        Mesh& mesh = meshes[i];
        if (mesh.skin.buffer) vmaDestroyBuffer(allocator, mesh.skin.buffer, mesh.skin.allocation);
        mesh.skin = {};
        mesh.skinAddress = 0;
    }
    vkDestroyPipeline(device, skinningPipeline, nullptr);
    vkDestroyPipelineLayout(device, skinningLayout, nullptr);
}

void VulkanRenderer::Impl::setMeshSkin(MeshHandle handle, std::span<const VertexSkin> skin) {
    if (handle.id >= kMaxMeshes || !meshReady[handle.id].load(std::memory_order_acquire))
        throw std::runtime_error("setMeshSkin: there's no such mesh");
    Mesh& mesh = meshes[handle.id];
    if (mesh.skin.buffer) throw std::runtime_error("setMeshSkin: the mesh is skinned already");
    if (skin.empty()) return;
    mesh.skin = uploadBuffer(
        skin.data(), skin.size_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    mesh.vertexCount = static_cast<uint32_t>(skin.size());
    mesh.skinAddress = bufferAddress(mesh.skin.buffer);
}

void VulkanRenderer::Impl::drawSkinnedMesh(
    MeshHandle handle, const glm::mat4& transform, std::span<const glm::mat4> joints, TextureHandle texture,
    const glm::vec4& tint, const DrawMaterial& material, const RenderInstanceKey& key) {
    drawMesh(handle, transform, texture, tint, material, key, joints);
}

glm::vec4 VulkanRenderer::Impl::skinnedBounds(const Mesh& mesh, std::span<const glm::mat4> joints) {
    // Each joint moves the mesh's bounding sphere to J * center, at most J's largest scale
    // times its radius wide. A skinned vertex is a weighted average of where its joints
    // put it, so it lies inside a sphere around all of those.
    const glm::vec3 center(mesh.bounds);
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    for (const glm::mat4& joint : joints) {
        const glm::vec3 p(joint * glm::vec4(center, 1.0f));
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const glm::vec3 middle = (lo + hi) * 0.5f;
    float radius = 0.0f;
    for (const glm::mat4& joint : joints) {
        const float scale = std::sqrt(std::max(
            {glm::dot(glm::vec3(joint[0]), glm::vec3(joint[0])), glm::dot(glm::vec3(joint[1]), glm::vec3(joint[1])),
             glm::dot(glm::vec3(joint[2]), glm::vec3(joint[2]))}));
        radius =
            std::max(radius, glm::length(glm::vec3(joint * glm::vec4(center, 1.0f)) - middle) + scale * mesh.bounds.w);
    }
    if (!std::isfinite(radius)) return mesh.bounds;
    return glm::vec4(middle, radius);
}

void VulkanRenderer::Impl::addDraw(
    DrawList& list, uint32_t listIndex, DrawCommand& command, uint32_t mesh, std::span<const glm::mat4> joints) {
    if (!joints.empty()) {
        command.skinList = listIndex;
        command.skin = static_cast<uint32_t>(list.skins.size());
        list.skins.push_back({mesh, static_cast<uint32_t>(list.joints.size()), static_cast<uint32_t>(joints.size())});
        list.joints.insert(list.joints.end(), joints.begin(), joints.end());
    }
    list.commands.push_back(command);
}

void VulkanRenderer::Impl::prepareSkinning(Frame& f) {
    SW_PROFILE_SCOPE("Prepare skinning");
    skinDispatches.clear();
    skinnedVertexCount = 0;
    std::lock_guard lock(foreignDrawMutex);
    const uint32_t listCount = static_cast<uint32_t>(drawLists.size()) + 1;
    auto list = [&](uint32_t i) -> DrawList& { return i < drawLists.size() ? drawLists[i] : foreignDraws; };

    // Where each list's joints and skinned vertices start.
    std::vector<uint32_t> jointBase(listCount), skinBase(listCount);
    std::vector<uint32_t> vertexOffsets;   // per skin job, flattened in list order
    size_t joints = 0, vertices = 0;
    for (uint32_t i = 0; i < listCount; ++i) {
        const DrawList& l = list(i);
        jointBase[i] = static_cast<uint32_t>(joints);
        skinBase[i] = static_cast<uint32_t>(vertexOffsets.size());
        joints += l.joints.size();
        for (const SkinJob& job : l.skins) {
            vertexOffsets.push_back(static_cast<uint32_t>(vertices));
            vertices += meshes[job.mesh].vertexCount;
        }
    }
    if (vertexOffsets.empty()) return;
    // Draws with a previous pose (G09) are skinned again with last frame's joints, after the rest.
    auto meshOf = [&](const DrawCommand& draw) { return list(draw.skinList).skins[draw.skin].mesh; };
    std::vector<uint32_t> previousJointOffsets, previousOffsets;   // per previous skin
    for (const PreviousSkin& skin : previousSkins) {
        previousJointOffsets.push_back(static_cast<uint32_t>(joints));
        joints += skin.jointCount;
        previousOffsets.push_back(static_cast<uint32_t>(vertices));
        vertices += meshes[meshOf(gatheredDraws[skin.draw])].vertexCount;
    }

    // Grow this frame's buffers when needed; the GPU is done with them (beginFrame waited).
    VmaAllocator allocator = context->allocator();
    if (joints > f.jointCapacity) {
        size_t capacity = std::max<size_t>(f.jointCapacity, 1024);
        while (capacity < joints) capacity *= 2;
        if (f.jointBuffer.buffer) vmaDestroyBuffer(allocator, f.jointBuffer.buffer, f.jointBuffer.allocation);
        VmaAllocationInfo info{};
        f.jointBuffer = createBuffer(
            capacity * sizeof(glm::mat4),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, &info);
        f.joints = static_cast<glm::mat4*>(info.pMappedData);
        f.jointAddress = bufferAddress(f.jointBuffer.buffer);
        f.jointCapacity = capacity;
    }
    if (vertices > f.skinnedCapacity) {
        size_t capacity = std::max<size_t>(f.skinnedCapacity, 65536);
        while (capacity < vertices) capacity *= 2;
        if (f.skinnedBuffer.buffer) vmaDestroyBuffer(allocator, f.skinnedBuffer.buffer, f.skinnedBuffer.allocation);
        f.skinnedBuffer = createBuffer(
            capacity * sizeof(Vertex), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            0);
        f.skinnedAddress = bufferAddress(f.skinnedBuffer.buffer);
        f.skinnedCapacity = capacity;
        f.skinnedState = {};
    }

    for (uint32_t i = 0; i < listCount; ++i) {
        const DrawList& l = list(i);
        if (!l.joints.empty())
            std::memcpy(f.joints + jointBase[i], l.joints.data(), l.joints.size() * sizeof(glm::mat4));
        for (uint32_t k = 0; k < l.skins.size(); ++k) {
            const SkinJob& job = l.skins[k];
            const Mesh& mesh = meshes[job.mesh];
            SkinDispatch dispatch;
            dispatch.push.source = mesh.vertexAddress;
            dispatch.push.skin = mesh.skinAddress;
            dispatch.push.joints = f.jointAddress + VkDeviceAddress(jointBase[i] + job.firstJoint) * sizeof(glm::mat4);
            dispatch.push.target = f.skinnedAddress + VkDeviceAddress(vertexOffsets[skinBase[i] + k]) * sizeof(Vertex);
            dispatch.push.vertexCount = mesh.vertexCount;
            dispatch.push.jointCount = job.jointCount;
            skinDispatches.push_back(dispatch);
        }
    }
    for (size_t s = 0; s < previousSkins.size(); ++s) {
        const PreviousSkin& skin = previousSkins[s];
        DrawCommand& draw = gatheredDraws[skin.draw];
        const Mesh& mesh = meshes[meshOf(draw)];
        const uint32_t firstJoint = previousJointOffsets[s];
        std::memcpy(
            f.joints + firstJoint, previousJoints.data() + skin.firstJoint, skin.jointCount * sizeof(glm::mat4));
        SkinDispatch dispatch;
        dispatch.push.source = mesh.vertexAddress;
        dispatch.push.skin = mesh.skinAddress;
        dispatch.push.joints = f.jointAddress + VkDeviceAddress(firstJoint) * sizeof(glm::mat4);
        dispatch.push.target = f.skinnedAddress + VkDeviceAddress(previousOffsets[s]) * sizeof(Vertex);
        dispatch.push.vertexCount = mesh.vertexCount;
        dispatch.push.jointCount = skin.jointCount;
        skinDispatches.push_back(dispatch);
        draw.data.previousVertexBuffer = dispatch.push.target;
    }
    vk::check(
        vmaFlushAllocation(allocator, f.jointBuffer.allocation, 0, joints * sizeof(glm::mat4)), "vmaFlushAllocation");

    for (DrawCommand& draw : gatheredDraws) {
        if (draw.skin == kNoSkin) continue;
        draw.data.vertexBuffer =
            f.skinnedAddress + VkDeviceAddress(vertexOffsets[skinBase[draw.skinList] + draw.skin]) * sizeof(Vertex);
    }
    skinnedVertexCount = static_cast<uint32_t>(vertices);
}

void VulkanRenderer::Impl::recordSkinning(VkCommandBuffer cmd) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, skinningPipeline);
    for (const SkinDispatch& dispatch : skinDispatches) {
        vkCmdPushConstants(cmd, skinningLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dispatch.push), &dispatch.push);
        vkCmdDispatch(cmd, (dispatch.push.vertexCount + 63) / 64, 1, 1);
    }
    // The render graph orders the scene and shadow passes' vertex reads after this.
}

}   // namespace shockwave
