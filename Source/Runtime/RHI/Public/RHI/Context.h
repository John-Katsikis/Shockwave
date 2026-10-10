#pragma once

#include <RHI/Common.h>

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
    VmaAllocator allocator() const { return m_allocator; }
    float maxAnisotropy() const { return m_maxAnisotropy; }
    uint32_t maxBindlessTextures() const { return m_maxBindlessTextures; }

private:
    vkb::Instance m_instance;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    vkb::Device m_device;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamily = 0;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    float m_maxAnisotropy = 1.0f;
    uint32_t m_maxBindlessTextures = 0;
};

}   // namespace shockwave::vk
