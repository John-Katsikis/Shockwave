# ozz-animation: skeletal animation runtime (sampling, blending, local-to-model) and its
# offline builders, which turn the clips imported from models into compressed runtime
# animations when a model loads. Only the libraries are built: no tools, samples or
# tests, and no FBX or glTF importers (Shockwave imports models with Assimp).
set(ozz_build_tools OFF CACHE BOOL "" FORCE)
set(ozz_build_fbx OFF CACHE BOOL "" FORCE)
set(ozz_build_gltf OFF CACHE BOOL "" FORCE)
set(ozz_build_samples OFF CACHE BOOL "" FORCE)
set(ozz_build_howtos OFF CACHE BOOL "" FORCE)
set(ozz_build_tests OFF CACHE BOOL "" FORCE)
set(ozz_build_postfix OFF CACHE BOOL "" FORCE)
# The DLL C runtime on Windows, like everything else (ozz defaults to the static one).
set(ozz_build_msvc_rt_dll ON CACHE BOOL "" FORCE)
FetchContent_Declare(ozz
    GIT_REPOSITORY https://github.com/guillaumeblanc/ozz-animation.git
    GIT_TAG 0.16.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ozz)
# ozz builds with warnings as errors; a newer compiler than it was tested with mustn't
# break Shockwave's build over a warning in ozz's own code.
foreach(target ozz_base ozz_animation ozz_animation_offline)
    if(MSVC)
        target_compile_options(${target} PRIVATE /WX-)
    else()
        target_compile_options(${target} PRIVATE -Wno-error)
    endif()
    set_target_properties(${target} PROPERTIES FOLDER "ThirdParty/ozz")
endforeach()
