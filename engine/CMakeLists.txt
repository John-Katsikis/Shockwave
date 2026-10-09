add_library(shockwave STATIC
    src/core/AssetFolder.cpp
    src/core/Assets.cpp
    src/core/Image.cpp
    src/core/Jobs.cpp
    src/input/Input.cpp
    src/render/Camera.cpp
    src/render/Mesh.cpp
    src/scene/Components.cpp
    src/scene/Hierarchy.cpp
    src/scene/SceneFile.cpp
    src/scene/Schedule.cpp
    src/scene/Systems.cpp
    src/scene/World.cpp
    src/ui/Gui.cpp
    src/ui/Theme.cpp
    src/Engine.cpp
    src/Window.cpp
    src/Renderer.cpp
    src/vulkan/Context.cpp
    src/vulkan/Swapchain.cpp
    src/vulkan/Pipeline.cpp
    src/vulkan/Vma.cpp
    ${SHOCKWAVE_IMGUI_BACKENDS}
)
target_include_directories(shockwave
    PUBLIC include
    PRIVATE src ${imgui_SOURCE_DIR}/backends
)
target_include_directories(shockwave SYSTEM PRIVATE ${stb_image_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(shockwave
    PUBLIC SDL3::SDL3 glm::glm Threads::Threads imgui
    PRIVATE Vulkan::Headers volk vk-bootstrap::vk-bootstrap GPUOpen::VulkanMemoryAllocator
)
# Vulkan clip space has depth 0..1; GLM must produce matching projection matrices.
target_compile_definitions(shockwave PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)
target_compile_definitions(shockwave PRIVATE VK_NO_PROTOTYPES
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    IMGUI_IMPL_VULKAN_USE_VOLK
    $<$<CONFIG:Debug>:SHOCKWAVE_VALIDATION=1>
)

if(MSVC)
    target_compile_options(shockwave PRIVATE /W4)
else()
    target_compile_options(shockwave PRIVATE -Wall -Wextra -Wpedantic)
    # Third-party implementation files: their warnings aren't ours to fix.
    set_source_files_properties(src/vulkan/Vma.cpp src/core/Image.cpp ${SHOCKWAVE_IMGUI_BACKENDS} PROPERTIES COMPILE_OPTIONS "-w")
endif()

shockwave_compile_shaders(shockwave
    shaders/mesh.vert
    shaders/mesh.frag
)
