# Animation: skeletal animation with ozz-animation. The Animator component, the clips
# imported with models, cross-fades, and game code's AnimationWorld. Poses are written
# to ModelPose (Engine/Pose.h) for renderWorld to draw. See docs/Animation.md and
# cmake/Ozz.cmake.
shockwave_module(Animation
    PUBLIC_DEPS Shockwave::Engine
    PRIVATE_DEPS ozz_animation ozz_animation_offline)
# ozz's SIMD types in templates trip GCC's -Wignored-attributes; harmless.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(ShockwaveAnimation PRIVATE -Wno-ignored-attributes)
endif()
