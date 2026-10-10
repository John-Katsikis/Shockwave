# The sample project's C++ game module, linked into the editor and the player.
set(SHOCKWAVE_GAME_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Source/SampleGame.cpp" CACHE FILEPATH "C++ gameplay module source")
add_library(ShockwaveGame STATIC ${SHOCKWAVE_GAME_SOURCE})
target_link_libraries(ShockwaveGame PUBLIC Shockwave::GameFramework)
shockwave_warnings(ShockwaveGame)
# The project this module belongs to: the player runs it when given no project.
set(SHOCKWAVE_GAME_PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/Sample.swproject" CACHE FILEPATH "The .swproject of the game module")
target_compile_definitions(ShockwaveGame INTERFACE SHOCKWAVE_GAME_PROJECT="${SHOCKWAVE_GAME_PROJECT}")
