# Multiplayer: the simulation side of networking. Network settings (.swnetwork), the
# network manifest and compatibility digests, the NetworkObject and NetworkSpawnPoint
# components, and the registry of replication profiles, commands and events that game
# modules fill. No Steam or transport code: sessions use Network's ITransport. The physics
# components it checks (RigidBody, CharacterController) are Engine data, so it needs no
# Physics dependency yet.
shockwave_module(Multiplayer PUBLIC_DEPS Shockwave::Engine Shockwave::Network)
if(SHOCKWAVE_STEAMWORKS)
    # Only so networkBuild() can say the build has Steam; Multiplayer never calls it.
    target_compile_definitions(ShockwaveMultiplayer PRIVATE SHOCKWAVE_ENABLE_STEAM=1
                               SHOCKWAVE_STEAMWORKS_VERSION="${SHOCKWAVE_STEAMWORKS_VERSION}")
endif()
