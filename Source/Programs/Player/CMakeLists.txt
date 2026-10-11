# The standalone player: runs a project's scenes with its game module, and the
# command-line tools (--check, --schema, --screenshot) used by people, CI and AI agents.
file(GLOB_RECURSE sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Private/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/Private/*.h)
add_executable(player ${sources})
target_include_directories(player PRIVATE Private)
target_link_libraries(player PRIVATE Shockwave::GameFramework)
target_compile_definitions(player PRIVATE SHOCKWAVE_GAME_PROJECT="${SHOCKWAVE_GAME_PROJECT}")
shockwave_link_plugins(player)
shockwave_warnings(player)
target_link_libraries(player PRIVATE Shockwave::Packaging)
