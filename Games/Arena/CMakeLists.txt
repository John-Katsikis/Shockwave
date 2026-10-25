shockwave_game_module(arena)

if(BUILD_TESTING)
    add_executable(arena_graphics_test Tests/GraphicsMenuTest.cpp Source/ArenaGraphicsMenu.cpp)
    target_include_directories(arena_graphics_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_link_libraries(arena_graphics_test PRIVATE Shockwave::GameFramework)
    shockwave_warnings(arena_graphics_test)
    add_test(NAME arena_graphics COMMAND arena_graphics_test ${CMAKE_CURRENT_SOURCE_DIR})
    # Batch 9 gate: the online menu through the real document, over the Steam mock.
    add_executable(arena_online_menu_test Tests/OnlineMenuTest.cpp Source/ArenaOnlineMenu.cpp)
    target_include_directories(arena_online_menu_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_link_libraries(arena_online_menu_test PRIVATE Shockwave::GameFramework Shockwave::SteamServices)
    shockwave_warnings(arena_online_menu_test)
    add_test(NAME arena_online_menu COMMAND arena_online_menu_test ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(arena_online_menu PROPERTIES TIMEOUT 120)
    # Exercise the arena shooter against real Jolt collision/character simulation, no GPU.
    set(ARENA_GAME_SOURCES Source/ArenaGame.cpp Source/ArenaGraphicsMenu.cpp Source/ArenaOnlineMenu.cpp Source/ArenaPresentation.cpp
        Source/ArenaSimulation.cpp)
    add_executable(arena_test Tests/ArenaTest.cpp ${ARENA_GAME_SOURCES})
    target_include_directories(arena_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_link_libraries(arena_test PRIVATE Shockwave::GameFramework)
    shockwave_warnings(arena_test)
    add_test(NAME arena COMMAND arena_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(arena PROPERTIES ENVIRONMENT "SDL_AUDIO_DRIVER=dummy" TIMEOUT 60)
    # Batch 6 gate: a host and eleven clients on the multiplayer scene, cheating clients,
    # and frame-rate independent input (Tests/ArenaNetworkTest.cpp).
    add_executable(arena_network_test Tests/ArenaNetworkTest.cpp ${ARENA_GAME_SOURCES} Source/ArenaNetwork.cpp)
    target_include_directories(arena_network_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_link_libraries(arena_network_test PRIVATE Shockwave::GameFramework)
    shockwave_warnings(arena_network_test)
    add_test(NAME network_arena COMMAND arena_network_test ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(network_arena PROPERTIES ENVIRONMENT "SDL_AUDIO_DRIVER=dummy" TIMEOUT 300)
    add_test(NAME arena_cli COMMAND ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:player>
        -DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/Arena.swproject
        -DSCENE=${CMAKE_CURRENT_SOURCE_DIR}/Scenes/arena.swscene
        -DMP_SCENE=${CMAKE_CURRENT_SOURCE_DIR}/Scenes/arena-mp.swscene
        -DTEMP=${CMAKE_CURRENT_BINARY_DIR}/ArenaCLI
        -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ArenaCheck.cmake)
    set_tests_properties(arena_cli PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;SDL_AUDIO_DRIVER=dummy" TIMEOUT 60)
    # Multiplayer settings, manifest and schema on a copy of Arena (Tests/NetworkCheck.cmake).
    add_test(NAME network_cli COMMAND ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:player>
        -DPROJECT_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DSCHEMA=${CMAKE_SOURCE_DIR}/docs/NetworkSchema.md
        -DTEMP=${CMAKE_CURRENT_BINARY_DIR}/NetworkCLI
        -DSTEAMWORKS=${SHOCKWAVE_STEAMWORKS}
        -P ${CMAKE_SOURCE_DIR}/Tests/NetworkCheck.cmake)
    set_tests_properties(network_cli PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;SDL_AUDIO_DRIVER=dummy" TIMEOUT 120)
    # Batch 9 gate: a Steam game's package (AppID 480, developer files, the runtime, offline
    # fallback) moved and run from elsewhere (Tests/SteamPackageCheck.cmake).
    if(SHOCKWAVE_MODULAR AND NOT WIN32)
        add_test(NAME package_steam COMMAND ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:player>
            -DPROJECT_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DTEMP=${CMAKE_CURRENT_BINARY_DIR}/SteamPackage
            -DSTEAMWORKS=${SHOCKWAVE_STEAMWORKS}
            -DSTEAM_RUNTIME=$<IF:$<PLATFORM_ID:Darwin>,libsteam_api.dylib,libsteam_api.so>
            -P ${CMAKE_SOURCE_DIR}/Tests/SteamPackageCheck.cmake)
        set_tests_properties(package_steam PROPERTIES
            ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;SDL_AUDIO_DRIVER=dummy" TIMEOUT 300 LABELS gpu)
    endif()

endif()
