# Asset optimisation (docs/Optimisation.md), used by the Assets module only.
#
# meshoptimizer: reorders triangles for the GPU's vertex cache and builds levels of detail.
# Basis Universal: compresses textures once into small .ktx2 files that turn into the GPU's
# own compressed formats (BC7 on desktop) in milliseconds when loaded.

set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MESHOPT_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG v1.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(meshoptimizer)
set_target_properties(meshoptimizer PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Basis Universal's repository is mostly test images and web demos (over 400 MB), so only
# the three folders the library needs are checked out, leaving out its CMakeLists (which
# builds command line tools and examples); the library is built below from its source list.
find_package(Git REQUIRED)
FetchContent_Declare(basisu
    DOWNLOAD_COMMAND
        ${GIT_EXECUTABLE} clone --quiet --depth 1 --branch v2_50 --filter=blob:none --no-checkout
            https://github.com/BinomialLLC/basis_universal.git <SOURCE_DIR>
    COMMAND ${GIT_EXECUTABLE} -C <SOURCE_DIR> sparse-checkout set --no-cone /encoder/ /transcoder/ /zstd/ /LICENSE
    COMMAND ${GIT_EXECUTABLE} -C <SOURCE_DIR> checkout --quiet
)
# The sparse checkout has no CMakeLists.txt at its root, so this only downloads it.
FetchContent_MakeAvailable(basisu)

set(basisu_dir ${basisu_SOURCE_DIR})
add_library(basisu STATIC
    ${basisu_dir}/encoder/basisu_backend.cpp
    ${basisu_dir}/encoder/basisu_basis_file.cpp
    ${basisu_dir}/encoder/basisu_comp.cpp
    ${basisu_dir}/encoder/basisu_enc.cpp
    ${basisu_dir}/encoder/basisu_etc.cpp
    ${basisu_dir}/encoder/basisu_frontend.cpp
    ${basisu_dir}/encoder/basisu_gpu_texture.cpp
    ${basisu_dir}/encoder/basisu_pvrtc1_4.cpp
    ${basisu_dir}/encoder/basisu_resampler.cpp
    ${basisu_dir}/encoder/basisu_resample_filters.cpp
    ${basisu_dir}/encoder/basisu_ssim.cpp
    ${basisu_dir}/encoder/basisu_uastc_enc.cpp
    ${basisu_dir}/encoder/basisu_bc7e_scalar.cpp
    ${basisu_dir}/encoder/basisu_dds_export.cpp
    ${basisu_dir}/encoder/basisu_bc7enc.cpp
    ${basisu_dir}/encoder/jpgd.cpp
    ${basisu_dir}/encoder/basisu_kernels_sse.cpp
    ${basisu_dir}/encoder/basisu_bc15_spmd.cpp
    ${basisu_dir}/encoder/basisu_bc15_spmd_sse.cpp
    ${basisu_dir}/encoder/basisu_opencl.cpp
    ${basisu_dir}/encoder/pvpngreader.cpp
    ${basisu_dir}/encoder/basisu_uastc_hdr_4x4_enc.cpp
    ${basisu_dir}/encoder/basisu_astc_hdr_6x6_enc.cpp
    ${basisu_dir}/encoder/basisu_astc_hdr_common.cpp
    ${basisu_dir}/encoder/basisu_astc_ldr_common.cpp
    ${basisu_dir}/encoder/basisu_astc_ldr_encode.cpp
    ${basisu_dir}/encoder/basisu_astc_ldr_fencode.cpp
    ${basisu_dir}/encoder/basisu_xbc7_encode.cpp
    ${basisu_dir}/encoder/basisu_tinyexr.cpp
    ${basisu_dir}/encoder/3rdparty/android_astc_decomp.cpp
    ${basisu_dir}/transcoder/basisu_transcoder.cpp
    ${basisu_dir}/zstd/zstd.c
)
target_include_directories(basisu SYSTEM PUBLIC ${basisu_dir})
# KTX2 with Zstandard supercompression; no SSE4.1 or OpenCL, so it runs on any x86-64 or
# ARM CPU. Basis asks for -fno-strict-aliasing.
target_compile_definitions(basisu
    PUBLIC BASISD_SUPPORT_KTX2_ZSTD=1 BASISD_SUPPORT_KTX2=1
    PRIVATE BASISU_SUPPORT_SSE=0 BASISU_SUPPORT_OPENCL=0 BASISU_SUPPORT_ASTCENC=0)
set_target_properties(basisu PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_STANDARD 17 FOLDER "ThirdParty")
if(MSVC)
    target_compile_options(basisu PRIVATE /w)
else()
    target_compile_options(basisu PRIVATE -w -fno-strict-aliasing)
endif()
find_package(Threads REQUIRED)
target_link_libraries(basisu PRIVATE Threads::Threads)
