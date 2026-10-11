# Steam Audio (Valve, Apache 2.0): HRTF binaural rendering, speaker panning and the
# direct-path filters behind spatial audio (docs/SpatialAudio.md). Valve ships it
# prebuilt, so the engine compiles against its header only and opens libphonon at
# runtime. Without the library the engine falls back to simple stereo panning, so
# turning this off, or deleting libphonon from build/bin, never breaks a build or a run.
option(SHOCKWAVE_STEAM_AUDIO "Spatial audio through Steam Audio (libphonon, loaded at runtime)" ON)
add_library(steam_audio_headers INTERFACE)
if(SHOCKWAVE_STEAM_AUDIO)
    FetchContent_Declare(steamaudio
        URL https://github.com/ValveSoftware/steam-audio/releases/download/v4.8.1/steamaudio_4.8.1.zip
        URL_HASH SHA256=4a0aa5ec1176f38f0b0993a37c2259d9e86f27e22d5e24f83ec4c3cb9a1d5449
    )
    FetchContent_MakeAvailable(steamaudio)
    target_include_directories(steam_audio_headers SYSTEM INTERFACE ${steamaudio_SOURCE_DIR}/include)
    target_compile_definitions(steam_audio_headers INTERFACE SHOCKWAVE_HAS_STEAM_AUDIO=1)
    # The prebuilt library for this platform, copied next to the executables.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        set(steamaudio_library ${steamaudio_SOURCE_DIR}/lib/linux-x64/libphonon.so)
    elseif(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(steamaudio_library ${steamaudio_SOURCE_DIR}/lib/windows-x64/phonon.dll)
    elseif(APPLE)
        set(steamaudio_library ${steamaudio_SOURCE_DIR}/lib/osx/libphonon.dylib)
    endif()
    if(steamaudio_library AND EXISTS ${steamaudio_library})
        file(COPY ${steamaudio_library} DESTINATION ${CMAKE_BINARY_DIR}/bin)
    else()
        message(STATUS "Steam Audio has no prebuilt library for this platform; spatial audio uses simple panning")
    endif()
    # Its licenses travel with it: Apache 2.0 for Steam Audio, and the notices of the
    # libraries inside libphonon.
    file(COPY ${CMAKE_CURRENT_LIST_DIR}/licenses/SteamAudio-LICENSE.md ${steamaudio_SOURCE_DIR}/THIRDPARTY.md
         DESTINATION ${CMAKE_BINARY_DIR}/bin/licenses/SteamAudio)
else()
    target_compile_definitions(steam_audio_headers INTERFACE SHOCKWAVE_HAS_STEAM_AUDIO=0)
endif()
