# Core: the job system, logging, frame pacing, profiling, plugin loading and other
# foundations every module may use. No engine dependencies.
find_package(Threads REQUIRED)
shockwave_module(Core PUBLIC_DEPS glm::glm Threads::Threads PRIVATE_DEPS ${CMAKE_DL_LIBS})
# SHOCKWAVE_MODULAR tells Core/Plugin.h whether plugins are libraries or linked in.
target_compile_definitions(ShockwaveCore PUBLIC SHOCKWAVE_MODULAR=$<BOOL:${SHOCKWAVE_MODULAR}>)
# Vulkan clip space has depth 0..1; GLM must produce matching projection matrices.
target_compile_definitions(ShockwaveCore PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)
if(SHOCKWAVE_PROFILE)
    target_link_libraries(ShockwaveCore PUBLIC Tracy::TracyClient)
    target_compile_definitions(ShockwaveCore PUBLIC SHOCKWAVE_PROFILE=1)
endif()
