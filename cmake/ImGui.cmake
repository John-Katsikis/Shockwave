# Dear ImGui (docking branch) for the editor and in-game debug UI, plus ImGuizmo for
# the editor's transform gizmos. Neither ships a CMake build we can use, so the targets
# are defined here. The SDL3 and Vulkan backends are compiled into the engine, which
# owns the window and the GPU.

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9b-docking
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG 18cef5e031d8c6973d80284c67f60549fafd78c1
    SOURCE_SUBDIR do-not-add   # use the sources only, not its CMakeLists
)
FetchContent_MakeAvailable(imgui imguizmo)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/misc/cpp)

add_library(imguizmo STATIC ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp)
target_include_directories(imguizmo SYSTEM PUBLIC ${imguizmo_SOURCE_DIR}/src)
target_link_libraries(imguizmo PUBLIC imgui)

if(NOT MSVC)
    target_compile_options(imgui PRIVATE -w)
    target_compile_options(imguizmo PRIVATE -w)
endif()

set(SHOCKWAVE_IMGUI_BACKENDS
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

# The UI font, copied next to the executables (build/bin/fonts).
file(COPY ${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf DESTINATION ${CMAKE_BINARY_DIR}/bin/fonts)

# UI themes, copied next to the executables as a fallback. The engine prefers
# themes/ in the working folder, so run from the project root to edit them live.
file(COPY ${CMAKE_SOURCE_DIR}/themes DESTINATION ${CMAKE_BINARY_DIR}/bin)
