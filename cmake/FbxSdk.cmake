# Autodesk FBX SDK (proprietary): an optional importer for .fbx models (F1b,
# docs/Assets.md#importing-fbx-with-the-autodesk-fbx-sdk). Assimp reads .fbx without it.
#
# Off by default, and then nothing here looks for, downloads or links any SDK file. The
# SDK is never fetched or committed, and CI builds without it: whoever turns this on
# downloads it from Autodesk (https://aps.autodesk.com/developer/overview/fbx-sdk),
# accepts its licence and installs it themselves.
#
#   -DSHOCKWAVE_FBXSDK=ON -DSHOCKWAVE_FBXSDK_ROOT=/path/to/FBX/SDK/2020.3.7
#
# where the folder holds include/fbxsdk.h and lib/. Turning it on makes the imported
# target FbxSdk::fbxsdk, which the Assets module links with SHOCKWAVE_HAS_FBXSDK=1, and
# copies the SDK's shared library next to the executables. Packages are refused from such
# a build (Packager.cpp), so a game never ships the SDK.
option(SHOCKWAVE_FBXSDK "Import .fbx through the Autodesk FBX SDK (download it yourself)" OFF)
set(SHOCKWAVE_FBXSDK_ROOT "" CACHE PATH "The FBX SDK's install folder")

if(NOT SHOCKWAVE_FBXSDK)
    return()
endif()

set(root "${SHOCKWAVE_FBXSDK_ROOT}")
if(root STREQUAL "")
    message(FATAL_ERROR "SHOCKWAVE_FBXSDK is ON but SHOCKWAVE_FBXSDK_ROOT is empty. Download the FBX SDK from "
                        "https://aps.autodesk.com/developer/overview/fbx-sdk, install it, and pass "
                        "-DSHOCKWAVE_FBXSDK_ROOT=<the folder holding include/fbxsdk.h>, or turn SHOCKWAVE_FBXSDK off.")
endif()
if(NOT EXISTS "${root}/include/fbxsdk.h")
    message(FATAL_ERROR "SHOCKWAVE_FBXSDK_ROOT=${root} has no include/fbxsdk.h. Point it at the FBX SDK's "
                        "install folder (the one holding include/ and lib/).")
endif()

# The shared library for the target being built, release flavour. Layouts differ between
# SDK versions (lib/gcc/x64/release, lib/vs2022/x64/release, lib/x64/release,
# lib/clang/release), so every library of the right name under lib/ is a candidate, and
# a release one for this architecture wins.
if(WIN32)
    set(runtime_name "libfbxsdk.dll")
    set(import_name "libfbxsdk.lib")
elseif(APPLE)
    set(runtime_name "libfbxsdk.dylib")
else()
    set(runtime_name "libfbxsdk.so")
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(arch_pattern "/(arm64|aarch64)/")
else()
    set(arch_pattern "/(x64|x86_64|amd64)/")
endif()
function(fbxsdk_pick out name)
    file(GLOB_RECURSE found "${root}/lib/*/${name}")
    set(best "")
    set(best_score -1)
    foreach(candidate IN LISTS found)
        set(score 0)
        string(TOLOWER "${candidate}" lower)
        if(lower MATCHES "/release/")
            math(EXPR score "${score} + 2")
        endif()
        if(lower MATCHES "${arch_pattern}")
            math(EXPR score "${score} + 1")
        endif()
        if(score GREATER best_score)
            set(best "${candidate}")
            set(best_score ${score})
        endif()
    endforeach()
    set(${out} "${best}" PARENT_SCOPE)
endfunction()
fbxsdk_pick(runtime "${runtime_name}")
set(import "")
if(WIN32)
    fbxsdk_pick(import "${import_name}")
endif()
if(runtime STREQUAL "" OR (WIN32 AND import STREQUAL ""))
    message(FATAL_ERROR "SHOCKWAVE_FBXSDK: no ${runtime_name} ${import_name} under ${root}/lib. Use a complete FBX SDK "
                        "install for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}.")
endif()

add_library(FbxSdk::fbxsdk SHARED IMPORTED GLOBAL)
set_target_properties(FbxSdk::fbxsdk PROPERTIES
    IMPORTED_LOCATION "${runtime}"
    INTERFACE_INCLUDE_DIRECTORIES "${root}/include"
    INTERFACE_COMPILE_DEFINITIONS "FBXSDK_SHARED")   # the SDK's headers need it to use the shared library
if(import)
    set_target_properties(FbxSdk::fbxsdk PROPERTIES IMPORTED_IMPLIB "${import}")
endif()
# Executables load it from next to themselves (the engine's $ORIGIN / @loader_path rpath).
file(COPY "${runtime}" DESTINATION ${CMAKE_BINARY_DIR}/bin)
message(STATUS "Autodesk FBX SDK: ${runtime}")
