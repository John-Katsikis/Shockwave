#include <RHI/Context.h>

#include <Core/Log.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdio>

namespace shockwave::vk {

namespace {

SW_LOG_CATEGORY(LogVulkan);

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const shockwave::LogLevel level =
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)     ? shockwave::LogLevel::Error
        : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? shockwave::LogLevel::Warning
                                                                       : shockwave::LogLevel::Info;
    shockwave::logMessage(LogVulkan, level, data->pMessage);
    return VK_FALSE;
}

}   // namespace

Context::Context(SDL_Window* window) {
    check(volkInitialize(), "Loading the Vulkan library (is a Vulkan driver installed?)");

    Uint32 extCount = 0;
    const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&extCount);
    if (!exts) {
        throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
    }

    vkb::InstanceBuilder instanceBuilder(vkGetInstanceProcAddr);
    instanceBuilder.set_app_name("Shockwave").set_engine_name("Shockwave").require_api_version(1, 3, 0);
    for (Uint32 i = 0; i < extCount; ++i) instanceBuilder.enable_extension(exts[i]);
#ifdef SHOCKWAVE_VALIDATION
    // Validation layers catch API misuse early. Install them from your distro
    // (vulkan-validation-layers); without them the engine still runs.
    instanceBuilder.request_validation_layers(true)
        .set_debug_callback(debugCallback)
        .set_debug_messenger_severity(
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT);
#endif
    auto instanceResult = instanceBuilder.build();
    if (!instanceResult) {
        throw std::runtime_error("Creating the Vulkan instance failed: " + instanceResult.error().message());
    }
    m_instance = instanceResult.value();
    volkLoadInstance(m_instance.instance);

    if (!SDL_Vulkan_CreateSurface(window, m_instance.instance, nullptr, &m_surface)) {
        throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());
    }

    // The modern baseline, all core in Vulkan 1.3 and supported by every current desktop GPU
    // and by MoltenVK:
    //  - dynamic rendering: no render pass / framebuffer objects
    //  - synchronization2: clearer barriers and submits
    //  - timeline semaphores: one counter paces CPU/GPU work instead of piles of fences
    //  - buffer device address + scalar layout: shaders read buffers through raw GPU pointers
    //  - descriptor indexing: one bindless table holds every texture, updated while in use
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    // Alpha-tested material shaders compile discard to SPIR-V demote.
    features13.shaderDemoteToHelperInvocation = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.timelineSemaphore = VK_TRUE;
    features12.bufferDeviceAddress = VK_TRUE;
    features12.scalarBlockLayout = VK_TRUE;
    features12.descriptorIndexing = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;

    VkPhysicalDeviceFeatures features10{};
    features10.samplerAnisotropy = VK_TRUE;

    vkb::PhysicalDeviceSelector selector(m_instance);
    auto physicalResult = selector.set_surface(m_surface)
                              .set_minimum_version(1, 3)
                              .set_required_features_13(features13)
                              .set_required_features_12(features12)
                              .set_required_features(features10)
                              .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
                              .select();
    if (!physicalResult) {
        throw std::runtime_error(
            "No GPU supports what Shockwave needs (Vulkan 1.3): " + physicalResult.error().message());
    }

    auto deviceResult = vkb::DeviceBuilder(physicalResult.value()).build();
    if (!deviceResult) {
        throw std::runtime_error("Creating the Vulkan device failed: " + deviceResult.error().message());
    }
    m_device = deviceResult.value();
    volkLoadDevice(m_device.device);

    VkPhysicalDeviceVulkan12Properties props12{};
    props12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &props12;
    vkGetPhysicalDeviceProperties2(physicalDevice(), &props);
    m_maxAnisotropy = props.properties.limits.maxSamplerAnisotropy;
    m_maxBindlessTextures = std::min<uint32_t>(props12.maxDescriptorSetUpdateAfterBindSampledImages, 65536);

    auto queue = m_device.get_queue(vkb::QueueType::graphics);
    auto family = m_device.get_queue_index(vkb::QueueType::graphics);
    if (!queue || !family) {
        throw std::runtime_error("The GPU has no graphics queue");
    }
    m_graphicsQueue = queue.value();
    m_graphicsQueueFamily = family.value();

    VmaVulkanFunctions vmaFunctions{};
    vmaFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vmaFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    allocatorInfo.instance = m_instance.instance;
    allocatorInfo.physicalDevice = physicalDevice();
    allocatorInfo.device = m_device.device;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    allocatorInfo.pVulkanFunctions = &vmaFunctions;
    check(vmaCreateAllocator(&allocatorInfo, &m_allocator), "vmaCreateAllocator");
}

Context::~Context() {
    if (m_allocator) vmaDestroyAllocator(m_allocator);
    vkb::destroy_device(m_device);
    if (m_surface) vkDestroySurfaceKHR(m_instance.instance, m_surface, nullptr);
    vkb::destroy_instance(m_instance);
}

}   // namespace shockwave::vk
