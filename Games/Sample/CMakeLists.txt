# The sample project's C++ game module, linked into the editor and the player.
set(SHOCKWAVE_GAME_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Source/SampleGame.cpp" CACHE FILEPATH "C++ gameplay module source")
add_library(ShockwaveGame STATIC ${SHOCKWAVE_GAME_SOURCE})
target_link_libraries(ShockwaveGame PUBLIC Shockwave::GameFramework)
shockwave_warnings(ShockwaveGame)
