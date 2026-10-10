# Shockwave Editor: projects and scene editing on top of the engine, with a Dear ImGui interface.
add_executable(editor
    src/main.cpp
    src/AssetBrowser.cpp
    src/ConsolePanel.cpp
    src/Editor.cpp
    src/HierarchyPanel.cpp
    src/InspectorPanel.cpp
    src/Preferences.cpp
    src/ProjectHub.cpp
    src/Scene.cpp
    src/ViewportPanel.cpp
    src/Workspace.cpp
)
target_link_libraries(editor PRIVATE shockwave shockwave_game imguizmo)
# Lets the project hub offer the sample project wherever the editor was built.
target_compile_definitions(editor PRIVATE SHOCKWAVE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

if(MSVC)
    target_compile_options(editor PRIVATE /W4)
else()
    target_compile_options(editor PRIVATE -Wall -Wextra -Wpedantic)
endif()
