# The Vulkan renderer plugin: bin/plugins/renderer_vulkan.so.
set(backend ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
shockwave_plugin(renderer vulkan
    DEPS Shockwave::Renderer Shockwave::RHI imgui
    SOURCES ${backend}
    NO_WARNINGS ${backend})
target_include_directories(renderer_vulkan PRIVATE ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(renderer_vulkan PRIVATE IMGUI_IMPL_VULKAN_USE_VOLK)
shockwave_compile_shaders(renderer_vulkan
    Shaders/mesh.vert
    Shaders/mesh.frag
    Shaders/mesh_skin.frag
    Shaders/skin.frag
    Shaders/hzb.comp
    Shaders/cull.comp
    Shaders/grid.vert
    Shaders/grid.frag
    Shaders/tonemap.vert
    Shaders/tonemap.frag
    Shaders/shadow.vert
    Shaders/shadow.frag
    Shaders/ui.vert
    Shaders/ui.frag
    Shaders/particle.vert
    Shaders/particle.frag
    Shaders/skinning.comp
    Shaders/clusters.comp
    Shaders/environment_sky.comp
    Shaders/environment_filter.comp
    Shaders/sky.vert
    Shaders/sky.frag
    Shaders/bloom.comp
    Shaders/fxaa.frag
    Shaders/prepass.frag
    Shaders/ssao.comp
    Shaders/decal.vert
    Shaders/decal.frag
    Shaders/reactive.frag
    Shaders/taa.comp
    Shaders/exposure_histogram.comp
    Shaders/exposure_adapt.comp
    Shaders/fog.comp
)
