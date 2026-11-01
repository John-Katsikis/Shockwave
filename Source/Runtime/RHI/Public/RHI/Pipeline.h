#pragma once

#include <RHI/Common.h>

#include <string>

namespace shockwave::vk {

VkShaderModule loadShaderModule(VkDevice device, const std::string& path);

struct GraphicsPipelineDesc {
    bool alphaBlend = false;   // standard "over" blending; never writes depth
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    std::string vertexShader;   // path to .spv
    std::string fragmentShader;   // path to .spv; empty: none (depth only)
    // Specialization constants for the fragment shader (variants of one shader); may be null.
    const VkSpecializationInfo* fragmentSpecialization = nullptr;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    // A second colour attachment (location 1), blended like the first: the surface
    // prepass's decal layers, the decal pass's second buffer (G08). Integer formats need
    // blending off.
    VkFormat secondColorFormat = VK_FORMAT_UNDEFINED;
    // A third (location 2): the surface prepass's motion vectors (G09). With it, the second
    // may be VK_FORMAT_UNDEFINED, an attachment the pass leaves out.
    VkFormat thirdColorFormat = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
    bool depthWrite = true;   // with a depth format: test always, write only if set
    // Reverse-Z: nearer is greater. EQUAL draws exactly the surfaces a depth prepass left (G03).
    VkCompareOp depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;
    // Depth bias set per draw with vkCmdSetDepthBias (shadow maps). Reverse-Z: negative
    // values push surfaces away from the viewer.
    bool dynamicDepthBias = false;
    // Blending for colors that are already multiplied by their alpha (game UI): source
    // times one, destination times one minus source alpha. Never writes depth.
    bool premultipliedAlpha = false;
    // Keeps the greater of what's there and what's drawn, per channel (the reactive mask,
    // G09). Never writes depth.
    bool maxBlend = false;
    // Adds what's drawn to what's there (the skin pass's diffuse light, A08). Never writes depth.
    bool additiveBlend = false;
};

// Pipeline for dynamic rendering with vertex pulling (no vertex input state) and
// reverse-Z depth. No color format: depth only (shadow maps). Viewport and scissor are dynamic so resizes don't rebuild it.
VkPipeline createGraphicsPipeline(VkDevice device, const GraphicsPipelineDesc& desc);

}   // namespace shockwave::vk
