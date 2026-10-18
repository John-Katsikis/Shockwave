# Engine modules, laid out like Unreal's: Source/<Kind>/<Module>/Public holds the
# module's API (included as <Module/Header.h>), Private holds everything else.
#
# shockwave_module(<Module>
#     [STATIC]                      always a static library (code only one plugin links)
#     [PUBLIC_DEPS <targets...>]    modules and libraries users of this module also see
#     [PRIVATE_DEPS <targets...>]   implementation-only dependencies
#     [SOURCES <files...>]          extra sources (third-party backends)
#     [NO_WARNINGS <files...>]      sources compiled without our warning flags)
#
# Creates the library Shockwave<Module> with the alias Shockwave::<Module>: shared in
# modular builds (SHOCKWAVE_MODULAR), static otherwise. A module may only depend on
# modules below it; CMake reports a cycle if one doesn't.

function(shockwave_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(shockwave_module name)
    cmake_parse_arguments(ARG "STATIC" "" "PUBLIC_DEPS;PRIVATE_DEPS;SOURCES;NO_WARNINGS" ${ARGN})
    file(GLOB_RECURSE private_sources CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/Private/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/Private/*.h)
    file(GLOB_RECURSE public_headers CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Public/*.h)
    set(target Shockwave${name})
    if(SHOCKWAVE_MODULAR AND NOT ARG_STATIC)
        add_library(${target} SHARED ${private_sources} ${public_headers} ${ARG_SOURCES})
    else()
        add_library(${target} STATIC ${private_sources} ${public_headers} ${ARG_SOURCES})
    endif()
    if(ARG_STATIC AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE -fno-gnu-unique)   # it ends up in a reloadable plugin
    endif()
    add_library(Shockwave::${name} ALIAS ${target})
    target_include_directories(${target}
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Public
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private)
    target_link_libraries(${target} PUBLIC ${ARG_PUBLIC_DEPS} PRIVATE ${ARG_PRIVATE_DEPS})
    set_target_properties(${target} PROPERTIES FOLDER "Shockwave/${name}")
    shockwave_warnings(${target})
    if(ARG_NO_WARNINGS AND NOT MSVC)
        set_source_files_properties(${ARG_NO_WARNINGS} TARGET_DIRECTORY ${target} PROPERTIES COMPILE_OPTIONS "-w")
    endif()
endfunction()

# Plugins: libraries the engine loads at runtime (Core/Plugin.h, docs/Plugins.md).
#
# shockwave_plugin(<Kind> <Name> [DEPS <targets...>] [SOURCES <files...>] [NO_WARNINGS <files...>])
#
# Builds Source/Plugins/<Folder>/Private/* into bin/plugins/<kind>_<name>.so (.dylib on
# macOS, .dll on Windows, as Core/Plugin.cpp expects) in modular builds. Otherwise it's a static library that executables link whole with
# shockwave_link_plugins, so its SHOCKWAVE_PLUGIN registration is kept.
function(shockwave_plugin_target target)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # GCC marks inline variables and template statics "unique", which makes the loader
        # refuse to ever unload the library; reloading needs it to.
        target_compile_options(${target} PRIVATE -fno-gnu-unique)
    endif()
    set_property(GLOBAL APPEND PROPERTY SHOCKWAVE_PLUGIN_TARGETS ${target})
endfunction()

function(shockwave_plugin kind name)
    cmake_parse_arguments(ARG "" "" "DEPS;SOURCES;NO_WARNINGS" ${ARGN})
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/Private/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/Private/*.h)
    set(target ${kind}_${name})
    if(SHOCKWAVE_MODULAR)
        add_library(${target} MODULE ${sources} ${ARG_SOURCES})
        set_target_properties(${target} PROPERTIES
            PREFIX "" SUFFIX ${CMAKE_SHARED_LIBRARY_SUFFIX} DEBUG_POSTFIX ""
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins)
    else()
        add_library(${target} STATIC ${sources} ${ARG_SOURCES})
    endif()
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private)
    target_link_libraries(${target} PRIVATE ${ARG_DEPS})
    set_target_properties(${target} PROPERTIES FOLDER "Shockwave/Plugins")
    shockwave_warnings(${target})
    if(ARG_NO_WARNINGS AND NOT MSVC)
        set_source_files_properties(${ARG_NO_WARNINGS} TARGET_DIRECTORY ${target} PROPERTIES COMPILE_OPTIONS "-w")
    endif()
    shockwave_plugin_target(${target})
endfunction()

# A project's C++ game module (GameFramework/GameModule.h).
#
# shockwave_game_module(<Id> [SOURCES <files...>] [DEPS <targets...>])
#
# Without SOURCES it builds every .cpp under the project's Source/ folder, and new files
# are picked up on the next build with no CMake edit. Builds bin/modules/game_<id>.so in modular builds, which the editor and player load
# for projects whose .swproject says `game_module "<id>"`, and the editor reloads when
# it's rebuilt. Otherwise a static library linked into the editor and player.
function(shockwave_game_module id)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
    if(NOT ARG_SOURCES)
        file(GLOB_RECURSE ARG_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp)
        if(NOT ARG_SOURCES)
            message(FATAL_ERROR "game module ${id}: no .cpp files in ${CMAKE_CURRENT_SOURCE_DIR}/Source")
        endif()
    endif()
    set(target game_${id})
    set(entry ${PROJECT_SOURCE_DIR}/Source/Runtime/GameFramework/Entry/GameModuleEntry.cpp)
    if(SHOCKWAVE_MODULAR)
        add_library(${target} MODULE ${ARG_SOURCES} ${entry})
        set_target_properties(${target} PROPERTIES
            PREFIX "" SUFFIX ${CMAKE_SHARED_LIBRARY_SUFFIX} DEBUG_POSTFIX ""
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/modules)
    else()
        add_library(${target} STATIC ${ARG_SOURCES} ${entry})
    endif()
    target_link_libraries(${target} PRIVATE Shockwave::GameFramework ${ARG_DEPS})
    # Static builds link every game into one executable: give its entry symbols unique names.
    string(MAKE_C_IDENTIFIER "${id}" symbol)
    target_compile_definitions(${target} PRIVATE SW_GAME_MODULE_ID=${symbol})
    set_target_properties(${target} PROPERTIES FOLDER "Games")
    shockwave_warnings(${target})
    shockwave_plugin_target(${target})
endfunction()

# Makes an executable able to use every plugin and game module: built before it in
# modular builds, linked in whole otherwise.
function(shockwave_link_plugins target)
    get_property(plugins GLOBAL PROPERTY SHOCKWAVE_PLUGIN_TARGETS)
    if(NOT plugins)
        return()
    endif()
    if(SHOCKWAVE_MODULAR)
        add_dependencies(${target} ${plugins})
    else()
        target_link_libraries(${target} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${plugins}>")
    endif()
endfunction()
