# Shockwave Editor: scene editing on top of the engine, with a Dear ImGui interface.
add_executable(editor
    src/main.cpp
    src/AssetBrowser.cpp
    src/Editor.cpp
    src/Scene.cpp
)
target_link_libraries(editor PRIVATE shockwave imguizmo)

if(MSVC)
    target_compile_options(editor PRIVATE /W4)
else()
    target_compile_options(editor PRIVATE -Wall -Wextra -Wpedantic)
endif()
