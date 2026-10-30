# xatlas: chart segmentation and packing for generated lightmap UVs (G05, docs/Lighting.md).
# A pinned upstream revision; only its single source file is built, statically and
# privately into the Lighting module, so no xatlas type reaches a public header.
FetchContent_Declare(xatlas
    GIT_REPOSITORY https://github.com/jpcy/xatlas.git
    GIT_TAG f700c7790aaa030e794b52ba7791a05c085faf0c
    SOURCE_SUBDIR do-not-add   # use the sources only, not its CMakeLists
)
FetchContent_MakeAvailable(xatlas)
add_library(xatlas STATIC ${xatlas_SOURCE_DIR}/source/xatlas/xatlas.cpp)
target_include_directories(xatlas SYSTEM PUBLIC ${xatlas_SOURCE_DIR}/source/xatlas)
set_target_properties(xatlas PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "ThirdParty")
if(NOT MSVC)
    target_compile_options(xatlas PRIVATE -w)
endif()
# Its MIT license ships with the binaries, next to the other libraries' (build/bin/licenses).
configure_file(${xatlas_SOURCE_DIR}/LICENSE ${CMAKE_BINARY_DIR}/bin/licenses/xatlas-License.txt COPYONLY)
