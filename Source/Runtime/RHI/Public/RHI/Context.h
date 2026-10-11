#pragma once

#include <RHI/Common.h>
#include <Renderer/GpuSelection.h>

#include <VkBootstrap.h>

struct SDL_Window;

namespace shockwave::vk {

// Instance, surface, GPU and logical device: everything that lives as long as the window.
class Context {
public:
    explicit Context(SDL_Window* window);
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    VkInstance instance() const { return m_instance.instance; }
    VkSurfaceKHR surface() const { return m_surface; }
    VkPhysicalDevice physicalDevice() const { return m_device.physical_device.physical_device; }
    VkDevice device() const { return m_device.device; }
    const vkb::Device& vkbDevice() const { return m_device; }

    VkQueue graphicsQueue() const { return m_graphicsQueue; }
    uint32_t graphicsQueueFamily() const { return m_graphicsQueueFamily; }
    const char* gpuName() const { return m_device.physical_device.name.c_str(); }
    // Every GPU found, which one was picked and why (Renderer/GpuSelection.h).
    const GpuChoice& gpuChoice() const { return m_gpuChoice; }
    VmaAllocator allocator() const { return m_allocator; }
    float maxAnisotropy() const { return m_maxAnisotropy; }
    uint32_t maxBindlessTextures() const { return m_maxBindlessTextures; }

    // Frame pacing support, all optional (see enablePresentTiming).
    enum class PresentWait { None, Version1, Version2 };
    PresentWait presentWait() const { return m_presentWait; }
    // Some drivers report present wait 2 for a surface but then refuse swapchains that use
    // it (seen on XWayland). The swapchain calls this and retries without it.
    void disablePresentWait() const { m_presentWait = PresentWait::None; }
    bool fifoLatestReady() const { return m_fifoLatestReady; }   // VK_PRESENT_MODE_FIFO_LATEST_READY_KHR
    const char* presentTimingName() const;
    // Nanoseconds per GPU timestamp tick; 0 when the graphics queue has no timestamps.
    float timestampPeriod() const { return m_timestampPeriod; }

private:
    void enablePresentTiming(vkb::PhysicalDevice& physical, bool surfaceCaps2);
    VkPhysicalDevice chooseGpu(
        const VkPhysicalDeviceVulkan13Features& features13, const VkPhysicalDeviceVulkan12Features& features12,
        const VkPhysicalDeviceFeatures& features10);

    GpuChoice m_gpuChoice;
    vkb::Instance m_instance;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    vkb::Device m_device;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamily = 0;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    float m_maxAnisotropy = 1.0f;
    uint32_t m_maxBindlessTextures = 0;
    mutable PresentWait m_presentWait = PresentWait::None;
    bool m_fifoLatestReady = false;
    float m_timestampPeriod = 0.0f;
};

}   // namespace shockwave::vk
