# Network: platform-neutral multiplayer plumbing. The wire format and its message catalog,
# the transport interface, an in-process loopback transport with fault injection for
# tests, packet captures, and the online services interface. No Engine, Physics, SDL,
# renderer or Steam dependency: those adapt to this module, not the other way round.
shockwave_module(Network PUBLIC_DEPS Shockwave::Core)
if(WIN32)
    # BCryptGenRandom, for session nonces.
    target_link_libraries(ShockwaveNetwork PRIVATE bcrypt)
endif()
