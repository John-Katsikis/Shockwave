# RHI: the Vulkan layer (device, swapchain, pipelines, render graph). Only the Vulkan
# renderer plugin links it, so Vulkan never reaches game or editor code. Always static:
# it's built into the plugin, and reloading the plugin reloads it too.
shockwave_module(RHI STATIC
    PUBLIC_DEPS Shockwave::Platform Vulkan::Headers volk vk-bootstrap::vk-bootstrap GPUOpen::VulkanMemoryAllocator
    NO_WARNINGS ${CMAKE_CURRENT_SOURCE_DIR}/Private/Vma.cpp)
target_compile_definitions(ShockwaveRHI PUBLIC VK_NO_PROTOTYPES
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    $<$<CONFIG:Debug>:SHOCKWAVE_VALIDATION=1>)
