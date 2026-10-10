#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <stdexcept>
#include <string>

namespace shockwave::vk {

inline void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed (VkResult " + std::to_string(result) + ")");
    }
}

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
};

} // namespace shockwave::vk
