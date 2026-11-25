#include "RendererImpl.h"

// Material graphs (W4, docs/MaterialGraph.md). A graph's fragment shader is mesh.glsl with the
// graph's function spliced in (Assets/ShaderCompiler.h), so it draws with the same layout,
// push constants and specialization constants as mesh.frag, in pipelines of its own: the
// solid, mesh and depth EQUAL variants the engine's own surfaces have (glass, skin and water
// families don't take graphs). Its values go in a per-frame table like terrain's, pointed at
// by ShadingData::graph. With material_graphs off nothing here is built or filled.

namespace shockwave {

void VulkanRenderer::Impl::createSurfaceShader(SurfaceShaderHandle handle, std::span<const uint32_t> spirv) {
    if (!handle.id || handle.id >= kMaxSurfaceShaders) return;
    SurfaceShader& shader = surfaceShaders[handle.id];
    shader.spirv.assign(spirv.begin(), spirv.end());
    shader.pipelines = {};
    // The common case now, while loading: opaque, one-sided, not mirrored, with and without
    // the prepass.
    if (shadingGraphs.load(std::memory_order_relaxed)) {
        DrawCommand common;
        buildGraphPipeline(shader, graphPipelineIndex(common, false));
        buildGraphPipeline(shader, graphPipelineIndex(common, true));
    }
}

void VulkanRenderer::Impl::destroySurfaceShader(SurfaceShaderHandle handle) {
    if (!handle.id || handle.id >= kMaxSurfaceShaders) return;
    SurfaceShader& shader = surfaceShaders[handle.id];
    retire([device = context->device(), pipelines = shader.pipelines] {
        for (VkPipeline pipeline : pipelines)
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    });
    shader = {};
}

void VulkanRenderer::Impl::destroySurfaceShaders() {
    VkDevice device = context->device();
    for (SurfaceShader& shader : surfaceShaders) {
        for (VkPipeline pipeline : shader.pipelines)
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        shader = {};
    }
}

uint32_t VulkanRenderer::Impl::graphPipelineIndex(const DrawCommand& draw, bool depthEqual) {
    const uint32_t faces = (draw.mirrored ? 2u : 0u) + (draw.doubleSided ? 1u : 0u);
    if (depthEqual) return 12u + prepassIndex(draw);
    if (!draw.blend && draw.data.alphaCutoff < 0.0f) return faces;   // solid: no alpha test
    return 4u + (draw.blend ? 4u : 0u) + faces;
}

void VulkanRenderer::Impl::buildGraphPipeline(SurfaceShader& shader, uint32_t index) {
    if (shader.spirv.empty() || shader.pipelines[index]) return;
    vk::GraphicsPipelineDesc desc;
    desc.vertexShader = shaderPath("mesh.vert");
    desc.fragmentCode = &shader.spirv;
    desc.colorFormat = kSceneColorFormat;
    desc.depthFormat = kDepthFormat;
    desc.layout = pipelineLayout;
    const VkSpecializationMapEntry entries[2] = {{0, 0, sizeof(VkBool32)}, {1, sizeof(VkBool32), sizeof(VkBool32)}};
    VkBool32 constants[2] = {VK_FALSE, VK_FALSE};   // alpha test, receives AO
    VkSpecializationInfo spec{};
    uint32_t faces = 0;
    if (index < 4) {   // solid, as solidPipelines
        faces = index;
        spec = {1, entries, sizeof(VkBool32), constants};
        desc.fragmentSpecialization = &spec;
    } else if (index < 12) {   // as meshPipeline, mirroredPipelines and the blended ones
        faces = (index - 4) & 3u;
        desc.alphaBlend = ((index - 4) & 4u) != 0;
    } else {   // over the prepass, as depthEqualPipelines
        const uint32_t i = index - 12;
        faces = i & 3u;
        constants[0] = (i & 4u) ? VK_TRUE : VK_FALSE;
        constants[1] = VK_TRUE;
        spec = {2, entries, sizeof(constants), constants};
        desc.fragmentSpecialization = &spec;
        desc.depthWrite = false;
        desc.depthCompare = VK_COMPARE_OP_EQUAL;
    }
    desc.frontFace = (faces & 2u) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    desc.cullMode = (faces & 1u) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    shader.pipelines[index] = vk::createGraphicsPipeline(context->device(), desc);
}

void VulkanRenderer::Impl::ensureGraphPipelines(std::span<const DrawCommand> draws) {
    for (const DrawCommand& draw : draws) {
        if (!draw.surfaceShader) continue;
        SurfaceShader& shader = surfaceShaders[draw.surfaceShader];
        buildGraphPipeline(shader, graphPipelineIndex(draw, false));
        if (!draw.blend) buildGraphPipeline(shader, graphPipelineIndex(draw, true));
    }
}

void VulkanRenderer::Impl::fillGraphShading(const MaterialGraphShading& graph, GraphShadingData& out) {
    out = {};
    for (size_t i = 0; i < kGraphSlots; ++i) {
        out.params[i] = graph.params[i];
        out.textures[i] = textureSlot(graph.textures[i].id);
        out.samplers[i] = samplerSlot(graph.samplers[i]);
    }
}

void VulkanRenderer::Impl::ensureGraphCapacity(Frame& f, uint32_t count) {
    if (count <= f.graphCapacity) return;
    uint32_t capacity = std::max<uint32_t>(f.graphCapacity, 64);
    while (capacity < count) capacity *= 2;
    if (f.graphBuffer.buffer) vmaDestroyBuffer(context->allocator(), f.graphBuffer.buffer, f.graphBuffer.allocation);
    VmaAllocationInfo info{};
    f.graphBuffer = createBuffer(
        capacity * sizeof(GraphShadingData),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, &info);
    f.graphData = static_cast<GraphShadingData*>(info.pMappedData);
    f.graphAddress = bufferAddress(f.graphBuffer.buffer);
    f.graphCapacity = capacity;
}

}   // namespace shockwave
