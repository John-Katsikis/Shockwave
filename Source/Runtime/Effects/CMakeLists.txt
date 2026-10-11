# Effects: particle effects made in the Effekseer editor (.efkefc), simulated on a job
# and drawn by any renderer plugin as plain triangles (Renderer/Particles.h). The
# ParticleEffect component and game code's EffectsWorld. See docs/Effects.md and
# cmake/Effekseer.cmake.
shockwave_module(Effects
    PUBLIC_DEPS Shockwave::Engine
    PRIVATE_DEPS effekseer_runtime)
