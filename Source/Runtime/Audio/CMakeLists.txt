# Audio: SDL3 mixer and Play-session audio, layered above Engine/Assets. Spatial voices
# render through Steam Audio when libphonon is found at runtime (cmake/SteamAudio.cmake).
shockwave_module(Audio PUBLIC_DEPS Shockwave::Engine PRIVATE_DEPS steam_audio_headers)
