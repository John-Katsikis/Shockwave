shockwave_game_module(arena)

if(BUILD_TESTING)
    # Exercise the arena shooter against real Jolt collision/character simulation, no GPU.
    add_executable(arena_test Tests/ArenaTest.cpp Source/ArenaGame.cpp)
    target_include_directories(arena_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_link_libraries(arena_test PRIVATE Shockwave::GameFramework)
    shockwave_warnings(arena_test)
    add_test(NAME arena COMMAND arena_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(arena PROPERTIES ENVIRONMENT "SDL_AUDIO_DRIVER=dummy" TIMEOUT 60)
    add_test(NAME arena_cli COMMAND ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:player>
        -DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/Arena.swproject
        -DSCENE=${CMAKE_CURRENT_SOURCE_DIR}/Scenes/arena.swscene
        -DTEMP=${CMAKE_CURRENT_BINARY_DIR}/ArenaCLI
        -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ArenaCheck.cmake)
    set_tests_properties(arena_cli PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;SDL_AUDIO_DRIVER=dummy" TIMEOUT 60)

endif()
