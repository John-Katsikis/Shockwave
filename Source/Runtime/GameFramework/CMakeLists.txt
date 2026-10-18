# GameFramework: game modules and Play sessions (gameplay systems, physics, start/stop).
shockwave_module(GameFramework PUBLIC_DEPS Shockwave::Engine Shockwave::Physics Shockwave::Audio Shockwave::GameUI
                 Shockwave::Navigation Shockwave::Effects Shockwave::Animation Shockwave::Circuits
                 Shockwave::Multiplayer)
# SW_COMPONENT and SW_FIELD use __VA_OPT__, which MSVC only supports with its standard
# preprocessor; game modules include them, so they get the option too.
if(MSVC)
    target_compile_options(ShockwaveGameFramework PUBLIC /Zc:preprocessor)
endif()
