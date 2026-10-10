# Core: the job system and other foundations every module may use. No engine dependencies.
find_package(Threads REQUIRED)
shockwave_module(Core PUBLIC_DEPS glm::glm Threads::Threads)
# Vulkan clip space has depth 0..1; GLM must produce matching projection matrices.
target_compile_definitions(ShockwaveCore PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)
