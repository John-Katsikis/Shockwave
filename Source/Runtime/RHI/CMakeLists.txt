# RHI: the Vulkan layer (device, swapchain, pipelines, render graph). Only the Renderer
# links it, so Vulkan never reaches game or editor code.
shockwave_module(RHI
    PUBLIC_DEPS Shockwave::Platform Vulkan::Headers volk vk-bootstrap::vk-bootstrap GPUOpen::VulkanMemoryAllocator
    NO_WARNINGS ${CMAKE_CURRENT_SOURCE_DIR}/Private/Vma.cpp)
target_compile_definitions(ShockwaveRHI PUBLIC VK_NO_PROTOTYPES
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    $<$<CONFIG:Debug>:SHOCKWAVE_VALIDATION=1>)
