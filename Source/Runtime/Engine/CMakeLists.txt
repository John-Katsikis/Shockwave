# Engine: the World (entities, components, systems, hierarchy), scene files and the
# engine loop that ties the window, renderer, input, assets and UI together.
set(backend ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp)
shockwave_module(Engine
    PUBLIC_DEPS Shockwave::Core Shockwave::Platform Shockwave::Input Shockwave::Renderer
                Shockwave::Assets Shockwave::UI Shockwave::Projects
    SOURCES ${backend}
    NO_WARNINGS ${backend})
target_include_directories(ShockwaveEngine PRIVATE ${imgui_SOURCE_DIR}/backends)
