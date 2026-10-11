# Dear ImGui (docking branch) for the editor and in-game debug UI, plus ImGuizmo for
# the editor's transform gizmos. Neither ships a CMake build we can use, so the targets
# are defined here. The SDL3 backend is compiled into the Engine module (it owns the
# window events) and the Vulkan backend into the Renderer (it owns the GPU).

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
# The node editor behind the editor's Circuits graph (docs/Circuits.md). The develop
# branch, patched below for this Dear ImGui.
FetchContent_Declare(
    imgui_node_editor
    GIT_REPOSITORY https://github.com/thedmd/imgui-node-editor.git
    GIT_TAG b302971455b3719ec9b5fb94b2f92d27c62b9ff0
    SOURCE_SUBDIR do-not-add
)
FetchContent_MakeAvailable(imgui imguizmo imgui_node_editor)

# Shared in modular builds: the GUI context is global, and every module and plugin that
# draws or renders UI must see the same one.
if(SHOCKWAVE_MODULAR)
    set(imgui_type SHARED)
else()
    set(imgui_type STATIC)
endif()
add_library(imgui ${imgui_type}
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

# imgui-node-editor predates Dear ImGui 1.92, which added operator*(float, ImVec2) itself and
# dropped ImRect::Floor. Patched copies are compiled from the build folder, so the
# download stays as it is.
set(node_editor_dir ${CMAKE_BINARY_DIR}/imgui_node_editor)
set(node_editor_files crude_json.cpp crude_json.h imgui_bezier_math.h imgui_bezier_math.inl imgui_canvas.cpp
    imgui_canvas.h imgui_extra_math.h imgui_extra_math.inl imgui_node_editor.cpp imgui_node_editor.h
    imgui_node_editor_api.cpp imgui_node_editor_internal.h imgui_node_editor_internal.inl)
foreach(file ${node_editor_files})
    file(READ ${imgui_node_editor_SOURCE_DIR}/${file} text)
    if(file STREQUAL "imgui_extra_math.inl")
        string(REPLACE "inline ImVec2 operator*(const float lhs, const ImVec2& rhs)\n{\n    return ImVec2(lhs * rhs.x, lhs * rhs.y);\n}"
            "# if IMGUI_VERSION_NUM < 19000\ninline ImVec2 operator*(const float lhs, const ImVec2& rhs)\n{\n    return ImVec2(lhs * rhs.x, lhs * rhs.y);\n}\n# endif"
            text "${text}")
    endif()
    string(REGEX REPLACE "([A-Za-z_][A-Za-z0-9_>.-]*)\\.Floor\\(\\);" "\\1 = ImRect(ImFloor(\\1.Min), ImFloor(\\1.Max));"
        text "${text}")
    file(CONFIGURE OUTPUT ${node_editor_dir}/${file} CONTENT "${text}" @ONLY)
endforeach()
add_library(imgui_node_editor STATIC
    ${node_editor_dir}/crude_json.cpp ${node_editor_dir}/imgui_canvas.cpp
    ${node_editor_dir}/imgui_node_editor.cpp ${node_editor_dir}/imgui_node_editor_api.cpp)
target_include_directories(imgui_node_editor SYSTEM PUBLIC ${node_editor_dir})
target_link_libraries(imgui_node_editor PUBLIC imgui)

if(NOT MSVC)
    target_compile_options(imgui PRIVATE -w)
    target_compile_options(imguizmo PRIVATE -w)
    target_compile_options(imgui_node_editor PRIVATE -w)
endif()

# The UI fonts (Cousine is the monospaced default), copied next to the executables (build/bin/fonts).
file(COPY ${imgui_SOURCE_DIR}/misc/fonts/Cousine-Regular.ttf ${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf
     DESTINATION ${CMAKE_BINARY_DIR}/bin/fonts)

# UI themes, copied next to the executables as a fallback. The engine prefers
# themes/ in the working folder, so run from the project root to edit them live.
file(COPY ${CMAKE_SOURCE_DIR}/themes DESTINATION ${CMAKE_BINARY_DIR}/bin)
