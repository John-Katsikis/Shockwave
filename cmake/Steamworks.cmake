# Steamworks SDK (Valve, proprietary): Steam sign-in, lobbies, authentication and Steam
# Datagram Relay networking for the SteamServices module (docs/Networking.md).
#
# Off by default, and then nothing here looks for, downloads or links any SDK file: the
# engine builds and plays offline everywhere, ARM64 included. The SDK is never fetched or
# committed: whoever turns this on supplies their own copy from partner.steamgames.com.
#
#   -DSHOCKWAVE_STEAMWORKS=ON -DSHOCKWAVE_STEAMWORKS_SDK=/path/to/sdk
#
# where the folder holds public/steam/steam_api.h and redistributable_bin/. Turning it on
# defines SHOCKWAVE_ENABLE_STEAM for the modules that need to know, makes the imported
# target Steamworks::steam_api, and copies the SDK's runtime library next to the
# executables. SHOCKWAVE_STEAM_TESTS adds tests that need Steam running and signed in.
option(SHOCKWAVE_STEAMWORKS "Build the Steamworks adapter (needs SHOCKWAVE_STEAMWORKS_SDK)" OFF)
set(SHOCKWAVE_STEAMWORKS_SDK "" CACHE PATH "Steamworks SDK folder (the one holding public/steam/steam_api.h)")
option(SHOCKWAVE_STEAM_TESTS "Tests that need Steam running and signed in (manual)" OFF)

if(NOT SHOCKWAVE_STEAMWORKS)
    if(SHOCKWAVE_STEAM_TESTS)
        message(FATAL_ERROR "SHOCKWAVE_STEAM_TESTS needs SHOCKWAVE_STEAMWORKS=ON")
    endif()
    return()
endif()

set(sdk "${SHOCKWAVE_STEAMWORKS_SDK}")
if(sdk STREQUAL "")
    message(FATAL_ERROR "SHOCKWAVE_STEAMWORKS is ON but SHOCKWAVE_STEAMWORKS_SDK is empty. Download the "
                        "Steamworks SDK from https://partner.steamgames.com/downloads/list, unzip it, and pass "
                        "-DSHOCKWAVE_STEAMWORKS_SDK=<folder holding public/steam/steam_api.h>, or turn "
                        "SHOCKWAVE_STEAMWORKS off.")
endif()
if(NOT EXISTS "${sdk}/public/steam/steam_api.h")
    message(FATAL_ERROR "SHOCKWAVE_STEAMWORKS_SDK=${sdk} has no public/steam/steam_api.h. Point it at the "
                        "unzipped Steamworks SDK's top folder (the one holding public/ and redistributable_bin/).")
endif()

# The runtime for the target being built, not the machine building it. Valve ships
# 64-bit x86 libraries for Linux and Windows and a universal one for macOS.
set(runtime "")
set(import "")
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(runtime "${sdk}/redistributable_bin/linux64/libsteam_api.so")
elseif(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
    set(runtime "${sdk}/redistributable_bin/win64/steam_api64.dll")
    set(import "${sdk}/redistributable_bin/win64/steam_api64.lib")
elseif(APPLE)
    set(runtime "${sdk}/redistributable_bin/osx/libsteam_api.dylib")
else()
    message(FATAL_ERROR "SHOCKWAVE_STEAMWORKS: the Steamworks SDK has no runtime for ${CMAKE_SYSTEM_NAME} "
                        "${CMAKE_SYSTEM_PROCESSOR} (${CMAKE_SIZEOF_VOID_P}-byte pointers). Turn "
                        "SHOCKWAVE_STEAMWORKS off for this target; the engine works offline without it.")
endif()
if(NOT EXISTS "${runtime}" OR (import AND NOT EXISTS "${import}"))
    message(FATAL_ERROR "SHOCKWAVE_STEAMWORKS_SDK=${sdk} is missing ${runtime} ${import}. Use a complete, "
                        "unmodified Steamworks SDK.")
endif()

add_library(Steamworks::steam_api SHARED IMPORTED GLOBAL)
set_target_properties(Steamworks::steam_api PROPERTIES
    IMPORTED_LOCATION "${runtime}"
    INTERFACE_INCLUDE_DIRECTORIES "${sdk}/public")
if(import)
    set_target_properties(Steamworks::steam_api PROPERTIES IMPORTED_IMPLIB "${import}")
endif()
# Executables load it from next to themselves (the engine's $ORIGIN / @loader_path rpath).
file(COPY "${runtime}" DESTINATION ${CMAKE_BINARY_DIR}/bin)

# The SDK's version line, if its readme has one, for --network-info and the editor.
set(SHOCKWAVE_STEAMWORKS_VERSION "unknown")
file(GLOB readme "${sdk}/Readme.txt" "${sdk}/readme.txt")
if(readme)
    list(GET readme 0 readme)
    file(STRINGS "${readme}" versions REGEX "^v[0-9]+\\.[0-9]+")
    if(versions)
        list(GET versions 0 SHOCKWAVE_STEAMWORKS_VERSION)
        string(REGEX MATCH "^v[0-9]+\\.[0-9]+" SHOCKWAVE_STEAMWORKS_VERSION "${SHOCKWAVE_STEAMWORKS_VERSION}")
    endif()
endif()
message(STATUS "Steamworks SDK ${SHOCKWAVE_STEAMWORKS_VERSION}: ${sdk}")
