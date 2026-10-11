# Shockwave Editor: projects and scene editing on top of the engine, with a Dear ImGui interface.
file(GLOB_RECURSE sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Private/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/Private/*.h)
add_executable(editor ${sources})
target_include_directories(editor PRIVATE Private)
target_link_libraries(editor PRIVATE Shockwave::GameFramework imguizmo)
# The source folder lets the project hub offer the sample project wherever the editor was
# built; the build folder lets the editor rebuild game code (Build and reload game code).
target_compile_definitions(editor PRIVATE SHOCKWAVE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    SHOCKWAVE_BUILD_DIR="${CMAKE_BINARY_DIR}")
shockwave_warnings(editor)
shockwave_link_plugins(editor)
target_link_libraries(editor PRIVATE Shockwave::Packaging)
