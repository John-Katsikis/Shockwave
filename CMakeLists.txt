cmake_minimum_required(VERSION 3.24)
project(Shockwave VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Every executable lands in build/bin, with compiled shaders next to it in build/bin/shaders.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(FetchContent)

# SDL3: windowing, input, audio, timing. Built from source so no system package is needed.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
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
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(volk vk_bootstrap VulkanMemoryAllocator glm stb_image)

include(cmake/Shaders.cmake)
include(cmake/ImGui.cmake)

add_subdirectory(engine)
add_subdirectory(sandbox)
add_subdirectory(editor)

# Unit tests that need no GPU or window: ctest --test-dir build
enable_testing()
add_subdirectory(tests)
