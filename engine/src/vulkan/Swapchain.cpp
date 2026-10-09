#include "vulkan/Swapchain.h"

#include "vulkan/Context.h"

namespace shockwave::vk {

Swapchain::Swapchain(const Context& context, uint32_t width, uint32_t height, bool vsync)
    : m_context(context), m_vsync(vsync) {
    build(width, height);
}

Swapchain::~Swapchain() {
    destroyViews();
    vkb::destroy_swapchain(m_swapchain);
}

void Swapchain::recreate(uint32_t width, uint32_t height) {
    destroyViews();
    build(width, height);
}

void Swapchain::build(uint32_t width, uint32_t height) {
    vkb::SwapchainBuilder builder(m_context.vkbDevice());
    builder.set_desired_format({VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
        .add_fallback_format({VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
        .set_desired_extent(width, height)
        .set_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        .set_old_swapchain(m_swapchain);
    if (m_vsync) {
        builder.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR);
    } else {
        // Uncapped: mailbox renders flat out without tearing; immediate is the fallback.
        // FIFO is always available as the last resort.
        builder.set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
            .add_fallback_present_mode(VK_PRESENT_MODE_IMMEDIATE_KHR)
            .add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR);
    }

    auto result = builder.build();
    if (!result) {
        throw std::runtime_error("Creating the swapchain failed: " + result.error().message());
    }
    vkb::destroy_swapchain(m_swapchain);
    m_swapchain = result.value();

    m_images = m_swapchain.get_images().value();
    m_imageViews = m_swapchain.get_image_views().value();
}

void Swapchain::destroyViews() {
    m_swapchain.destroy_image_views(m_imageViews);
    m_imageViews.clear();
    m_images.clear();
}

} // namespace shockwave::vk
