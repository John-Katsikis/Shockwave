#include <RHI/Context.h>

#include <Core/Log.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace shockwave::vk {

namespace {

SW_LOG_CATEGORY(LogVulkan);

#ifdef SHOCKWAVE_VALIDATION
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
#endif

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
    // Needed to ask a surface whether it supports present wait 2 (frame pacing).
    auto systemInfo = vkb::SystemInfo::get_system_info(vkGetInstanceProcAddr);
    const bool surfaceCaps2 =
        systemInfo && systemInfo->is_extension_available(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    if (surfaceCaps2) instanceBuilder.enable_extension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
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

    vkb::PhysicalDevice physical = physicalResult.value();
    enablePresentTiming(physical, surfaceCaps2);
    VkPhysicalDeviceFeatures compression{};
    compression.textureCompressionBC = VK_TRUE;   // optional: textures fall back to RGBA8 without it
    m_textureCompressionBC = physical.enable_features_if_present(compression);

    auto deviceResult = vkb::DeviceBuilder(physical).build();
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
    m_timestampPeriod = props.properties.limits.timestampPeriod;
    m_maxBindlessTextures = std::min<uint32_t>(props12.maxDescriptorSetUpdateAfterBindSampledImages, 65536);

    auto queue = m_device.get_queue(vkb::QueueType::graphics);
    auto family = m_device.get_queue_index(vkb::QueueType::graphics);
    if (!queue || !family) {
        throw std::runtime_error("The GPU has no graphics queue");
    }
    m_graphicsQueue = queue.value();
    m_graphicsQueueFamily = family.value();
    // GPU timers need timestamps on the graphics queue.
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice(), &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice(), &familyCount, families.data());
    if (m_graphicsQueueFamily < familyCount && families[m_graphicsQueueFamily].timestampValidBits == 0)
        m_timestampPeriod = 0.0f;
    SW_LOG(LogVulkan, Info, "frame pacing: {}{}", presentTimingName(), m_fifoLatestReady ? ", FIFO latest ready" : "");

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

const char* Context::presentTimingName() const {
    switch (m_presentWait) {
    case PresentWait::Version2: return "present wait 2";
    case PresentWait::Version1: return "present wait";
    case PresentWait::None: break;
    }
    return "no present wait (estimated from the CPU)";
}

// Present wait tells the CPU when a frame actually reached the screen, which the low-latency
// mode and the frame statistics use. Prefers the newer present id 2 / present wait 2, which
// the surface must also support, and falls back to the original extensions. Every piece is
// optional: without them the engine paces from CPU timing.
void Context::enablePresentTiming(vkb::PhysicalDevice& physical, bool surfaceCaps2) {
    VkPhysicalDevicePresentId2FeaturesKHR id2{};
    id2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR;
    VkPhysicalDevicePresentWait2FeaturesKHR wait2{};
    wait2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR;
    VkPhysicalDevicePresentIdFeaturesKHR id1{};
    id1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    VkPhysicalDevicePresentWaitFeaturesKHR wait1{};
    wait1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR latest{};
    latest.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR;

    auto has = [&](const char* ext) { return physical.is_extension_present(ext); };
    const bool hasV2 = has(VK_KHR_PRESENT_ID_2_EXTENSION_NAME) && has(VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME);
    const bool hasV1 = has(VK_KHR_PRESENT_ID_EXTENSION_NAME) && has(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    const bool hasLatest = has(VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME);
    // Ask which features the device really has (only structs of present extensions may be chained).
    VkPhysicalDeviceFeatures2 query{};
    query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    void** next = &query.pNext;
    auto chain = [&](auto& feature) {
        *next = &feature;
        next = &feature.pNext;
    };
    if (hasV2) chain(id2), chain(wait2);
    if (hasV1) chain(id1), chain(wait1);
    if (hasLatest) chain(latest);
    vkGetPhysicalDeviceFeatures2(physical.physical_device, &query);
    id2.pNext = wait2.pNext = id1.pNext = wait1.pNext = latest.pNext = nullptr;

    bool surfaceV2 = false;
    if (hasV2 && id2.presentId2 && wait2.presentWait2 && surfaceCaps2) {
        VkSurfaceCapabilitiesPresentId2KHR capsId{};
        capsId.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR;
        VkSurfaceCapabilitiesPresentWait2KHR capsWait{};
        capsWait.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR;
        capsWait.pNext = &capsId;
        VkSurfaceCapabilities2KHR caps{};
        caps.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
        caps.pNext = &capsWait;
        VkPhysicalDeviceSurfaceInfo2KHR info{};
        info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
        info.surface = m_surface;
        if (vkGetPhysicalDeviceSurfaceCapabilities2KHR &&
            vkGetPhysicalDeviceSurfaceCapabilities2KHR(physical.physical_device, &info, &caps) == VK_SUCCESS)
            surfaceV2 = capsId.presentId2Supported && capsWait.presentWait2Supported;
    }
    if (surfaceV2) {
        id2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, nullptr, VK_TRUE};
        wait2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR, nullptr, VK_TRUE};
        physical.enable_extension_if_present(VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
        physical.enable_extension_if_present(VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME);
        physical.enable_extension_features_if_present(id2);
        physical.enable_extension_features_if_present(wait2);
        m_presentWait = PresentWait::Version2;
    } else if (hasV1 && id1.presentId && wait1.presentWait) {
        id1 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, nullptr, VK_TRUE};
        wait1 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, nullptr, VK_TRUE};
        physical.enable_extension_if_present(VK_KHR_PRESENT_ID_EXTENSION_NAME);
        physical.enable_extension_if_present(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        physical.enable_extension_features_if_present(id1);
        physical.enable_extension_features_if_present(wait1);
        m_presentWait = PresentWait::Version1;
    }
    if (hasLatest && latest.presentModeFifoLatestReady) {
        latest = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR, nullptr, VK_TRUE};
        physical.enable_extension_if_present(VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME);
        physical.enable_extension_features_if_present(latest);
        m_fifoLatestReady = true;
    }
}

}   // namespace shockwave::vk
