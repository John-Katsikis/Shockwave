# GLSL -> SPIR-V at build time.
# Uses glslangValidator from the system if there is one (pacman: glslang), otherwise
# builds glslang from source so a fresh machine needs nothing extra.

option(SHOCKWAVE_FETCH_GLSLANG "Always build glslang from source instead of using the system one" OFF)

if(NOT SHOCKWAVE_FETCH_GLSLANG)
    find_program(SHOCKWAVE_GLSLANG NAMES glslangValidator glslang)
endif()

if(SHOCKWAVE_GLSLANG)
    message(STATUS "Shockwave: compiling shaders with ${SHOCKWAVE_GLSLANG}")
    set(SHOCKWAVE_GLSLANG_COMMAND ${SHOCKWAVE_GLSLANG})
else()
    message(STATUS "Shockwave: no glslangValidator found, building glslang from source")
    FetchContent_Declare(
        glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG vulkan-sdk-1.4.363.0
        GIT_SHALLOW TRUE
    )
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glslang)
    set(SHOCKWAVE_GLSLANG_COMMAND $<TARGET_FILE:glslang-standalone>)
    set(SHOCKWAVE_GLSLANG_DEPENDS glslang-standalone)
endif()

set(SHOCKWAVE_SHADER_OUTPUT_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders)

# shockwave_compile_shaders(<target> <shader files...>)
# Compiles each shader to <name>.spv in build/bin/shaders and makes <target> depend on them.
function(shockwave_compile_shaders target)
    set(outputs)
    foreach(source ${ARGN})
        get_filename_component(name ${source} NAME)
        set(output ${SHOCKWAVE_SHADER_OUTPUT_DIR}/${name}.spv)
        add_custom_command(
            OUTPUT ${output}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SHOCKWAVE_SHADER_OUTPUT_DIR}
            COMMAND ${SHOCKWAVE_GLSLANG_COMMAND} -V --target-env vulkan1.3 -o ${output} ${CMAKE_CURRENT_SOURCE_DIR}/${source}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${source} ${SHOCKWAVE_GLSLANG_DEPENDS}
            COMMENT "Compiling shader ${name}"
            VERBATIM
        )
        list(APPEND outputs ${output})
    endforeach()
    add_custom_target(${target}_shaders DEPENDS ${outputs})
    add_dependencies(${target} ${target}_shaders)
endfunction()
