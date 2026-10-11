cmake_minimum_required(VERSION 3.24)
project(Shockwave VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Every executable lands in build/bin, with compiled shaders next to it in build/bin/shaders.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

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
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    # Executables find the engine's libraries next to themselves, so build/bin can move.
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
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
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
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
    GIT_TAG v5.6.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR Build
)
FetchContent_MakeAvailable(JoltPhysics)

include(cmake/AudioCodecs.cmake)
include(cmake/RmlUi.cmake)
include(cmake/Recast.cmake)

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

include(cmake/ShockwaveModule.cmake)

# Runtime modules and plugins first: the game module links against them, and the
# editor and player against all of it.
add_subdirectory(Source)
# Game folders to build: each has a CMakeLists.txt calling shockwave_game_module(<id>).
# Add your own game's folder here (a ;-separated list). The editor opens any project
# whose module is built.
set(SHOCKWAVE_GAMES "${CMAKE_CURRENT_SOURCE_DIR}/Games/Sample" CACHE STRING "Game module folders to build")
# The project the player runs when it's given none.
set(SHOCKWAVE_GAME_PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/Games/Sample/Sample.swproject"
    CACHE FILEPATH "The .swproject the player runs by default")
foreach(game ${SHOCKWAVE_GAMES})
    get_filename_component(game_name ${game} NAME)
    add_subdirectory(${game} ${CMAKE_BINARY_DIR}/Games/${game_name})
endforeach()
add_subdirectory(Source/Editor)
add_subdirectory(Source/Programs)

enable_testing()
add_subdirectory(Tests)
