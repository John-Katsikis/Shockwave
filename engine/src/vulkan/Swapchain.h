#pragma once

#include "vulkan/Common.h"

#include <VkBootstrap.h>

#include <vector>

namespace shockwave::vk {

class Context;

// The chain of images we render into and hand to the window system.
class Swapchain {
public:
    Swapchain(const Context& context, uint32_t width, uint32_t height, bool vsync);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Rebuild after a resize. The caller must make sure the GPU is done with the old images.
    void recreate(uint32_t width, uint32_t height);
    // Takes effect at the next recreate.
    void setVsync(bool vsync) { m_vsync = vsync; }
    bool vsync() const { return m_vsync; }

    VkSwapchainKHR handle() const { return m_swapchain.swapchain; }
    VkExtent2D extent() const { return m_swapchain.extent; }
    VkFormat format() const { return m_swapchain.image_format; }
    uint32_t imageCount() const { return static_cast<uint32_t>(m_images.size()); }
    VkImage image(uint32_t i) const { return m_images[i]; }
    VkImageView imageView(uint32_t i) const { return m_imageViews[i]; }

private:
    void build(uint32_t width, uint32_t height);
    void destroyViews();

    const Context& m_context;
    bool m_vsync;
    vkb::Swapchain m_swapchain;
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_imageViews;
};

} // namespace shockwave::vk
