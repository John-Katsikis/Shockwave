cmake_minimum_required(VERSION 3.24)
project(Shockwave VERSION 0.1.0 LANGUAGES C CXX)
# No configuration means no optimisation at all: games then run many times slower, and a
# package made from such a build ships that. Release unless one is asked for.
get_property(shockwave_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT shockwave_multi_config AND NOT CMAKE_BUILD_TYPE)
    message(STATUS "No CMAKE_BUILD_TYPE given: building Release (-DCMAKE_BUILD_TYPE=Debug for a debug build)")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Debug, Release, RelWithDebInfo or MinSizeRel" FORCE)
endif()
include(CTest)

# Sanitizer builds for the nightly CI job (docs/Testing.md#ci): "address,undefined" or
# "thread". Set before the dependencies so they're instrumented too (TSan needs that).
set(SHOCKWAVE_SANITIZE "" CACHE STRING "Build with these sanitizers: address,undefined or thread (GCC/Clang)")
if(SHOCKWAVE_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "SHOCKWAVE_SANITIZE is for GCC and Clang builds")
    endif()
    add_compile_options(-fsanitize=${SHOCKWAVE_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${SHOCKWAVE_SANITIZE})
    # Jolt frees jobs after an acquire fence, which TSan can't see; its TSan mode uses an
    # acq_rel decrement instead. Its own check misses GCC, so say so (empty, as Jolt does).
    if(SHOCKWAVE_SANITIZE MATCHES "thread")
        add_compile_definitions("JPH_TSAN_ENABLED=")
    endif()
endif()

# Every test of Shockwave's own has a timeout and a label CI selects by (docs/Testing.md):
# each folder with tests ends by calling this (deferred), and configuring stops when one
# is missing. SHOCKWAVE_TEST_TIMEOUT_SCALE multiplies every timeout for slow builds.
set(SHOCKWAVE_TEST_LABELS unit integration gpu timing installed online_mock online_real)
set(SHOCKWAVE_TEST_TIMEOUT_SCALE 1 CACHE STRING "Multiply every test's timeout (a whole number; sanitizer builds)")
function(shockwave_check_tests)
    get_property(tests DIRECTORY PROPERTY TESTS)
    list(JOIN SHOCKWAVE_TEST_LABELS ", " allowed)
    set(problems "")
    foreach(test IN LISTS tests)
        get_test_property(${test} TIMEOUT timeout)
        get_test_property(${test} LABELS labels)
        if(NOT timeout)
            list(APPEND problems "${test} has no TIMEOUT")
        elseif(NOT SHOCKWAVE_TEST_TIMEOUT_SCALE EQUAL 1)
            math(EXPR timeout "${timeout} * ${SHOCKWAVE_TEST_TIMEOUT_SCALE}")
            set_tests_properties(${test} PROPERTIES TIMEOUT ${timeout})
        endif()
        if(NOT labels)
            list(APPEND problems "${test} has no LABELS (one of: ${allowed})")
        endif()
        foreach(label IN LISTS labels)
            if(NOT label IN_LIST SHOCKWAVE_TEST_LABELS)
                list(APPEND problems "${test} has the label ${label}, which isn't one of: ${allowed}")
            endif()
        endforeach()
    endforeach()
    if(problems)
        list(JOIN problems "\n  " shown)
        message(FATAL_ERROR "Tests in ${CMAKE_CURRENT_SOURCE_DIR} need a TIMEOUT and a label:\n  ${shown}")
    endif()
endfunction()

# Authoring templates ship beside the tools, so creating projects does not depend on cwd.
file(GLOB_RECURSE project_templates CONFIGURE_DEPENDS LIST_DIRECTORIES false
     RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/Templates" "${CMAKE_CURRENT_SOURCE_DIR}/Templates/*")
foreach(template IN LISTS project_templates)
    configure_file("Templates/${template}" "${CMAKE_BINARY_DIR}/bin/templates/${template}" COPYONLY)
endforeach()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Every executable lands in build/bin, with compiled shaders next to it in build/bin/shaders.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# Visual Studio and Xcode build several configurations; keep each one's programs in
# build/bin too, rather than build/bin/<Config>, so they find their shaders.
foreach(config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${config} config)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config} ${CMAKE_BINARY_DIR}/bin)
endforeach()

# Modular builds make every engine module a shared library and load renderers and game
# code as plugins, so they can be swapped and hot reloaded (docs/Plugins.md). Off links
# everything into each executable, as Windows builds do until modules export their
# symbols there.
if(WIN32)
    set(modular_default OFF)
else()
    set(modular_default ON)
endif()
option(SHOCKWAVE_MODULAR "Engine modules as shared libraries; renderers and game code as reloadable plugins"
       ${modular_default})
if(SHOCKWAVE_MODULAR)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    foreach(config ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER ${config} config)
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${config} ${CMAKE_BINARY_DIR}/bin)
    endforeach()
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    # Executables find the engine's libraries next to themselves, so build/bin can move.
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
    if(APPLE)
        # The same on macOS, where that option does nothing: next to the binary, or one
        # folder up for plugins and game modules.
        set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
        set(CMAKE_INSTALL_RPATH "@loader_path;@loader_path/..")
    endif()
endif()


include(FetchContent)

# SDL3: windowing, input, audio, timing. Built from source so no system package is needed.
# Shared in modular builds, so every module uses the one copy.
if(SHOCKWAVE_MODULAR)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
else()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
endif()
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.18
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3)

# Vulkan headers come from source too, so building needs no Vulkan SDK.
# Running needs a Vulkan driver (Mesa or the GPU vendor's), which Linux desktops ship.
FetchContent_Declare(
    VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG vulkan-sdk-1.4.363.0
    GIT_SHALLOW TRUE
)
# volk: loads Vulkan at runtime, so the engine does not link against libvulkan directly.
FetchContent_Declare(
    volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG vulkan-sdk-1.4.363.0
    GIT_SHALLOW TRUE
)
# vk-bootstrap: cuts the instance, device and swapchain boilerplate.
FetchContent_Declare(
    vk_bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    GIT_TAG v1.4.363
    GIT_SHALLOW TRUE
)
# VMA: GPU memory allocation (buffers, images).
FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.3.0
    GIT_SHALLOW TRUE
)
# GLM: vectors, matrices and quaternions.
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE
)
# stb_image: decodes PNG, JPEG and friends for texture import. One header.
FetchContent_Declare(
    stb_image
    URL https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image.h
    URL_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(VulkanHeaders)
# vk-bootstrap decides position independence itself; the Vulkan plugin is a shared library.
set(VK_BOOTSTRAP_POSITION_INDEPENDENT_CODE ${SHOCKWAVE_MODULAR} CACHE BOOL "" FORCE)
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(volk vk_bootstrap VulkanMemoryAllocator glm stb_image)
# volk must use the pinned fetched headers, not an accidental system Vulkan SDK.
target_link_libraries(volk PUBLIC Vulkan::Headers)

# All Assimp readers are available; exporters, tools and upstream tests aren't
# needed by the engine. Pin the release tag for reproducible imports.
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
# macOS ships zlib in its SDK, and Assimp's bundled copy doesn't compile against it (its
# zutil.h redefines fdopen). Other platforms build the bundled copy.
if(APPLE)
    set(ASSIMP_BUILD_ZLIB OFF CACHE BOOL "" FORCE)
else()
    set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
endif()
# Avoid an earlier system-minizip discovery leaking into the bundled build.
set(UNZIP_FOUND FALSE CACHE INTERNAL "Use Assimp bundled minizip" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG v6.0.5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(assimp)

# Jolt rigid-body physics. Build only the CPU library, without upstream apps,
# GPU compute backends, architecture-specific AVX assumptions, or global flags.
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
# The same C runtime as everything else on Windows (the DLL one); Jolt defaults to static.
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(CPP_EXCEPTIONS_ENABLED ON CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
foreach(feature USE_AVX USE_AVX2 USE_AVX512 USE_FMADD USE_F16C USE_LZCNT USE_TZCNT USE_SSE4_1 USE_SSE4_2)
    set(${feature} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.7.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR Build
)
FetchContent_MakeAvailable(JoltPhysics)

include(cmake/AudioCodecs.cmake)
include(cmake/RmlUi.cmake)
include(cmake/Recast.cmake)
include(cmake/SteamAudio.cmake)
include(cmake/Steamworks.cmake)
include(cmake/FbxSdk.cmake)
include(cmake/Ozz.cmake)
include(cmake/Xatlas.cmake)

# Tracy CPU profiler (Core/Profile.h). Off by default: with it off, nothing is fetched
# and the profiling macros compile away. On-demand mode records only while the Tracy
# profiler is connected, so a profiling build left running doesn't grow its memory.
option(SHOCKWAVE_PROFILE "Build with the Tracy profiler" OFF)
if(SHOCKWAVE_PROFILE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
    FetchContent_Declare(tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG v0.11.1
        GIT_SHALLOW TRUE
    )
    # One profiler shared by every module in modular builds.
    set(BUILD_SHARED_LIBS ${SHOCKWAVE_MODULAR})
    FetchContent_MakeAvailable(tracy)
    unset(BUILD_SHARED_LIBS)
endif()

include(cmake/Shaders.cmake)
include(cmake/ImGui.cmake)
include(cmake/Effekseer.cmake)

include(cmake/ShockwaveModule.cmake)

# Runtime modules and plugins first: the game module links against them, and the
# editor and player against all of it.
# MSVC: our sources are UTF-8, the biggest files need more sections than the default, and
# the macros in our headers (SW_COMPONENT's __VA_OPT__) need the standard preprocessor.
# Set here, after the dependencies, so it only reaches Shockwave's own targets: Assimp's
# zlib sets /source-charset, which /utf-8 can't be combined with.
if(MSVC)
    add_compile_options(/utf-8 /bigobj /Zc:__cplusplus /Zc:preprocessor)
endif()
# Sanitizer findings in third-party code we don't own (docs/Testing.md#third-party-code):
# basisu reads bits through deliberately unaligned pointers; Recast's hashes overflow ints on
# purpose; SDL3 copies zero display modes from a null array when a display has none
# (SDL_GetFullscreenDisplayModes). At the end of configuring, once every target exists
# (basisu is made in Source/).
function(shockwave_sanitizer_exemptions)
    foreach(sdl SDL3-shared SDL3-static)
        if(TARGET ${sdl})
            target_compile_options(${sdl} PRIVATE -fno-sanitize=nonnull-attribute)
        endif()
    endforeach()
    if(TARGET basisu)
        target_compile_options(basisu PRIVATE -fno-sanitize=alignment)
    endif()
    foreach(recast Recast Detour DetourCrowd DetourTileCache DebugUtils)
        if(TARGET ${recast})
            target_compile_options(${recast} PRIVATE -fno-sanitize=signed-integer-overflow)
        endif()
    endforeach()
endfunction()
if(SHOCKWAVE_SANITIZE)
    cmake_language(DEFER CALL shockwave_sanitizer_exemptions)
endif()
add_subdirectory(Source)
# Game folders to build: each has a CMakeLists.txt calling shockwave_game_module(<id>).
# SHOCKWAVE_GAMES is the whole list (it stays in the cache, so a build folder keeps it);
# SHOCKWAVE_EXTRA_GAMES adds folders to it. Relative folders are read from the engine
# folder. The editor opens any project whose module is built.
set(SHOCKWAVE_DEFAULT_GAMES "${CMAKE_CURRENT_SOURCE_DIR}/Games/Sample;${CMAKE_CURRENT_SOURCE_DIR}/Games/Arena;${CMAKE_CURRENT_SOURCE_DIR}/Games/ShippingDemo")
set(SHOCKWAVE_GAMES "${SHOCKWAVE_DEFAULT_GAMES}"
    CACHE STRING "Game module folders to build (absolute paths, separated by semicolons)")
set(SHOCKWAVE_EXTRA_GAMES ""
    CACHE STRING "More game folders to build as well as SHOCKWAVE_GAMES (absolute paths, separated by semicolons)")
# The project the player runs when it's given none.
set(SHOCKWAVE_GAME_PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/Games/Sample/Sample.swproject"
    CACHE FILEPATH "The .swproject the player runs by default")
set(shockwave_games "")
foreach(game IN LISTS SHOCKWAVE_GAMES SHOCKWAVE_EXTRA_GAMES)
    if(game STREQUAL "")
        continue()
    endif()
    get_filename_component(game "${game}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND shockwave_games "${game}")
endforeach()
list(REMOVE_DUPLICATES shockwave_games)
# A build folder configured before a built-in game existed keeps its old list; say so
# rather than skip the new game quietly.
foreach(game IN LISTS SHOCKWAVE_DEFAULT_GAMES)
    if(NOT game IN_LIST shockwave_games)
        message(WARNING "Shockwave: the built-in game ${game} is not being built, because "
            "SHOCKWAVE_GAMES leaves it out (it keeps the value this build folder was first "
            "given). Add it with -DSHOCKWAVE_EXTRA_GAMES=${game}, or reset the list with "
            "-USHOCKWAVE_GAMES.")
    endif()
endforeach()
set(shockwave_game_names "")
foreach(game IN LISTS shockwave_games)
    get_filename_component(game_name ${game} NAME)
    list(APPEND shockwave_game_names "${game_name}")
    # Two external projects can share a folder name; don't collide in the build tree.
    string(SHA256 game_hash "${game}")
    string(SUBSTRING "${game_hash}" 0 12 game_hash)
    add_subdirectory("${game}" "${CMAKE_BINARY_DIR}/Games/${game_name}-${game_hash}")
endforeach()
if(shockwave_game_names)
    list(JOIN shockwave_game_names ", " shockwave_game_names)
    message(STATUS "Shockwave games: ${shockwave_game_names}")
else()
    message(STATUS "Shockwave games: none")
endif()
add_subdirectory(Source/Editor)
add_subdirectory(Source/Programs)

if(BUILD_TESTING)
    add_subdirectory(Tests)
endif()
