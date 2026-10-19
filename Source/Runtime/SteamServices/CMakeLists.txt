# SteamServices: the optional Steamworks adapter (docs/Networking.md). Steam lifecycle and
# callbacks, lobbies, session-ticket authentication, and an ITransport over Steam Datagram
# Relay, all written against SteamApi so they're tested with MockSteamApi. Only
# Private/SteamSdk.cpp includes Steam headers, and only when the build has the SDK
# (-DSHOCKWAVE_STEAMWORKS=ON, cmake/Steamworks.cmake); otherwise SteamUnavailable.cpp
# stands in and nothing links Steam.
shockwave_module(SteamServices PUBLIC_DEPS Shockwave::Network)
if(SHOCKWAVE_STEAMWORKS)
    target_compile_definitions(ShockwaveSteamServices PRIVATE SHOCKWAVE_ENABLE_STEAM=1
                               SHOCKWAVE_STEAMWORKS_VERSION="${SHOCKWAVE_STEAMWORKS_VERSION}")
    target_link_libraries(ShockwaveSteamServices PRIVATE Steamworks::steam_api)
endif()
