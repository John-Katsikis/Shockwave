# Packaging: turns a project into a game folder for players (docs/Packaging.md).
shockwave_module(Packaging PUBLIC_DEPS Shockwave::Projects)
# The build this engine came from: packaging rebuilds it first, and warns about shipping
# a Debug build.
target_compile_definitions(ShockwavePackaging PRIVATE
    SHOCKWAVE_BUILD_DIR="${CMAKE_BINARY_DIR}"
    SHOCKWAVE_BUILD_CONFIG="$<CONFIG>"
    SHOCKWAVE_VERSION="${PROJECT_VERSION}")

# Packages ship build/bin/licenses: the engine's license and those of the libraries built
# into it. (RmlUi, FreeType and HarfBuzz add theirs in cmake/RmlUi.cmake; a new dependency
# adds its own the same way.)
set(license_dir ${CMAKE_BINARY_DIR}/bin/licenses)
configure_file(${PROJECT_SOURCE_DIR}/LICENSE ${license_dir}/Shockwave-LICENSE.txt COPYONLY)
foreach(entry
        "SDL3|sdl3|LICENSE.txt" "Jolt|joltphysics|LICENSE" "Assimp|assimp|LICENSE"
        "DearImGui|imgui|LICENSE.txt" "volk|volk|LICENSE.md" "vk-bootstrap|vk_bootstrap|LICENSE.txt"
        "VulkanMemoryAllocator|vulkanmemoryallocator|LICENSE.txt" "GLM|glm|copying.txt"
        "VulkanHeaders|vulkanheaders|LICENSE.md")
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 label)
    list(GET parts 1 dep)
    list(GET parts 2 file)
    if(EXISTS "${${dep}_SOURCE_DIR}/${file}")
        configure_file("${${dep}_SOURCE_DIR}/${file}" "${license_dir}/${label}-${file}" COPYONLY)
    endif()
endforeach()
