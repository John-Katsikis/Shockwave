# Shockwave Editor: projects and scene editing on top of the engine, with a Dear ImGui interface.
file(GLOB_RECURSE sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Private/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/Private/*.h)
add_executable(editor ${sources})
target_include_directories(editor PRIVATE Private)
target_link_libraries(editor PRIVATE Shockwave::GameFramework imguizmo imgui_node_editor)
# The source folder lets the project hub offer the sample project wherever the editor was
# built; the build folder lets the editor rebuild game code (Build and reload game code).
target_compile_definitions(editor PRIVATE SHOCKWAVE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    SHOCKWAVE_BUILD_DIR="${CMAKE_BINARY_DIR}")
shockwave_warnings(editor)
shockwave_link_plugins(editor)
target_link_libraries(editor PRIVATE Shockwave::Packaging Shockwave::SteamServices Shockwave::Lighting)

# Keep relative Markdown links usable after relocating build/bin. Run on each editor
# build so documentation-only edits are copied without requiring a relink.
add_custom_target(editor_docs
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/docs" "$<TARGET_FILE_DIR:editor>/docs"
    COMMENT "Copying editor documentation"
    VERBATIM)
add_dependencies(editor editor_docs)
