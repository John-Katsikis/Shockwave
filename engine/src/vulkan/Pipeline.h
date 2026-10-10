#pragma once

#include "vulkan/Common.h"

#include <string>

namespace shockwave::vk {

VkShaderModule loadShaderModule(VkDevice device, const std::string& path);

struct GraphicsPipelineDesc {
    bool alphaBlend = false;   // standard "over" blending; never writes depth
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    std::string vertexShader;     // path to .spv
    std::string fragmentShader;   // path to .spv
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
    bool depthWrite = true;    // with a depth format: test always, write only if set
};

// Pipeline for dynamic rendering with vertex pulling (no vertex input state) and
// reverse-Z depth. Viewport and scissor are dynamic so resizes don't rebuild it.
VkPipeline createGraphicsPipeline(VkDevice device, const GraphicsPipelineDesc& desc);

} // namespace shockwave::vk
