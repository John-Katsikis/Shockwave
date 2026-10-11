#include <RHI/Swapchain.h>

#include <RHI/Context.h>

#include <Core/Log.h>

#include <string>

namespace shockwave::vk {

namespace {
SW_LOG_CATEGORY(LogVulkan);
}   // namespace

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
        if (m_lowLatency && m_context.fifoLatestReady()) {
            builder.set_desired_present_mode(VK_PRESENT_MODE_FIFO_LATEST_READY_KHR)
                .add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR);
        } else {
            builder.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR);
        }
    } else {
        // Uncapped: mailbox renders flat out without tearing; immediate is the fallback.
        // FIFO is always available as the last resort.
        builder.set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
            .add_fallback_present_mode(VK_PRESENT_MODE_IMMEDIATE_KHR)
            .add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR);
    }
    const bool presentWait2 = m_context.presentWait() == Context::PresentWait::Version2;
    if (presentWait2) {
        builder.set_create_flags(static_cast<VkSwapchainCreateFlagBitsKHR>(
            VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR | VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR));
    }

    auto result = builder.build();
    if (!result && presentWait2) {
        // The surface said it supports present wait 2, but the driver refused it. Frame
        // pacing falls back to CPU timing rather than failing to start.
        SW_LOG(
            LogVulkan, Warning,
            "the driver refused a swapchain with present wait 2 (VkResult {}); pacing from CPU timing instead",
            static_cast<int>(result.vk_result()));
        m_context.disablePresentWait();
        builder.set_create_flags(static_cast<VkSwapchainCreateFlagBitsKHR>(0));
        result = builder.build();
    }
    if (!result) {
        throw std::runtime_error(
            "Creating the swapchain failed: " + result.error().message() + " (VkResult " +
            std::to_string(static_cast<int>(result.vk_result())) + ")");
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

}   // namespace shockwave::vk
