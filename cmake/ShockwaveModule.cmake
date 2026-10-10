# Engine modules, laid out like Unreal's: Source/<Kind>/<Module>/Public holds the
# module's API (included as <Module/Header.h>), Private holds everything else.
#
# shockwave_module(<Module>
#     [PUBLIC_DEPS <targets...>]    modules and libraries users of this module also see
#     [PRIVATE_DEPS <targets...>]   implementation-only dependencies
#     [SOURCES <files...>]          extra sources (third-party backends)
#     [NO_WARNINGS <files...>]      sources compiled without our warning flags)
#
# Creates the static library Shockwave<Module> with the alias Shockwave::<Module>.
# A module may only depend on modules below it; CMake reports a cycle if one doesn't.

function(shockwave_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(shockwave_module name)
    cmake_parse_arguments(ARG "" "" "PUBLIC_DEPS;PRIVATE_DEPS;SOURCES;NO_WARNINGS" ${ARGN})
    file(GLOB_RECURSE private_sources CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/Private/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/Private/*.h)
    file(GLOB_RECURSE public_headers CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Public/*.h)
    set(target Shockwave${name})
    add_library(${target} STATIC ${private_sources} ${public_headers} ${ARG_SOURCES})
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
