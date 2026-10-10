# Renderer: meshes, textures, lights and the frame (scene, tonemap, editor UI, present).
set(backend ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
shockwave_module(Renderer
    PUBLIC_DEPS Shockwave::Core Shockwave::Platform imgui
    PRIVATE_DEPS Shockwave::RHI
    SOURCES ${backend}
    NO_WARNINGS ${backend})
target_include_directories(ShockwaveRenderer PRIVATE ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(ShockwaveRenderer PRIVATE IMGUI_IMPL_VULKAN_USE_VOLK)
shockwave_compile_shaders(ShockwaveRenderer
    Shaders/mesh.vert
    Shaders/mesh.frag
    Shaders/grid.vert
    Shaders/grid.frag
    Shaders/tonemap.vert
    Shaders/tonemap.frag
    Shaders/shadow.vert
    Shaders/shadow.frag
)
