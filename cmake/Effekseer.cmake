# Effekseer: particle effects (docs/Effects.md). Only the runtime is built: the effect
# simulation (Dev/Cpp/Effekseer) and the geometry builders of its renderer
# (EffekseerRendererCommon). Shockwave draws that geometry through its own renderer
# plugins, so none of Effekseer's graphics backends, tools or submodules are needed.
# MIT licensed; the license file is in ${effekseer_SOURCE_DIR}/LICENSE.
# A shallow clone of one release, without the large example images kept in Git LFS.
set(ENV{GIT_LFS_SKIP_SMUDGE} 1)
FetchContent_Declare(effekseer
    GIT_REPOSITORY https://github.com/effekseer/Effekseer.git
    GIT_TAG 1.7.3.0
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ""
    SOURCE_SUBDIR Dev/Cpp/Effekseer/Effekseer   # no CMakeLists.txt there: fetch only
)
FetchContent_MakeAvailable(effekseer)

set(efk ${effekseer_SOURCE_DIR}/Dev/Cpp)
file(GLOB effekseer_runtime_sources
    ${efk}/Effekseer/Effekseer/*.cpp
    ${efk}/Effekseer/Effekseer/Material/*.cpp
    ${efk}/Effekseer/Effekseer/IO/*.cpp
    ${efk}/Effekseer/Effekseer/Parameter/*.cpp
    ${efk}/Effekseer/Effekseer/Utils/*.cpp
    ${efk}/Effekseer/Effekseer/SIMD/*.cpp
    ${efk}/Effekseer/Effekseer/Noise/*.cpp
    ${efk}/Effekseer/Effekseer/ForceField/*.cpp
    ${efk}/Effekseer/Effekseer/Model/*.cpp)
# The network viewer link (live editing from the Effekseer tool) stays out.
list(FILTER effekseer_runtime_sources EXCLUDE REGEX "Effekseer\\.(Client|Server|Socket)\\.cpp$")
list(APPEND effekseer_runtime_sources
    ${efk}/EffekseerRendererCommon/EffekseerRenderer.CommonUtils.cpp
    ${efk}/EffekseerRendererCommon/EffekseerRenderer.IndexBufferBase.cpp
    ${efk}/EffekseerRendererCommon/EffekseerRenderer.ModelRendererBase.cpp
    ${efk}/EffekseerRendererCommon/EffekseerRenderer.RenderStateBase.cpp
    ${efk}/EffekseerRendererCommon/EffekseerRenderer.Renderer.cpp
    ${efk}/EffekseerRendererCommon/EffekseerRenderer.Renderer_Impl.cpp
    ${efk}/EffekseerRendererCommon/EffekseerRenderer.RibbonRendererBase.cpp
    ${efk}/EffekseerRendererCommon/EffekseerRenderer.RingRendererBase.cpp
    ${efk}/EffekseerRendererCommon/EffekseerRenderer.SpriteRendererBase.cpp
    ${efk}/EffekseerRendererCommon/EffekseerRenderer.TrackRendererBase.cpp
    ${efk}/EffekseerRendererCommon/EffekseerRenderer.VertexBufferBase.cpp
    ${efk}/EffekseerRendererCommon/ModelLoader.cpp)
add_library(effekseer_runtime STATIC ${effekseer_runtime_sources})
target_include_directories(effekseer_runtime SYSTEM PUBLIC ${efk}/Effekseer ${efk}/EffekseerRendererCommon)
set_target_properties(effekseer_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "ThirdParty")
find_package(Threads REQUIRED)
target_link_libraries(effekseer_runtime PUBLIC Threads::Threads)
if(NOT MSVC)
    target_compile_options(effekseer_runtime PRIVATE -w)   # third-party code, built as is
    # Effekseer's ARM NEON math (Apple silicon, ARM Linux) gives NaN positions once an
    # effect moves, so ARM builds use its plain C++ path. PUBLIC: its headers inline that
    # math too, and every user must see the same path.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
        target_compile_options(effekseer_runtime PUBLIC -U__ARM_NEON -U__ARM_NEON__)
    endif()
endif()

# Effekseer's license, next to the other libraries' (build/bin/licenses).
configure_file(${effekseer_SOURCE_DIR}/LICENSE ${CMAKE_BINARY_DIR}/bin/licenses/Effekseer-LICENSE.txt COPYONLY)
