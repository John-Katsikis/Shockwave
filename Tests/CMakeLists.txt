add_executable(ecs_test EcsTest.cpp)
target_link_libraries(ecs_test PRIVATE Shockwave::Physics)
shockwave_warnings(ecs_test)
add_test(NAME ecs COMMAND ecs_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(prefab_test PrefabTest.cpp)
target_link_libraries(prefab_test PRIVATE Shockwave::Engine)
shockwave_warnings(prefab_test)
add_test(NAME prefab COMMAND prefab_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(ui_theme_test UiThemeTest.cpp)
target_link_libraries(ui_theme_test PRIVATE Shockwave::UI)
add_test(NAME ui_theme COMMAND ui_theme_test ${CMAKE_SOURCE_DIR}
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Packaging: settings files, and a whole package made and run through the CLI.
# Tests labelled gpu need a Vulkan driver; `ctest -LE gpu` runs the rest.
add_executable(package_test PackageTest.cpp)
target_link_libraries(package_test PRIVATE Shockwave::Packaging)
shockwave_warnings(package_test)
add_test(NAME package COMMAND package_test)
if(SHOCKWAVE_MODULAR AND NOT WIN32 AND TARGET game_sample)
    add_test(NAME package_cli COMMAND ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:player>
        -DPROJECT=${CMAKE_SOURCE_DIR}/Games/Sample/Sample.swproject
        -DTEMP=${CMAKE_CURRENT_BINARY_DIR}/PackageCLI
        -DLIBRARY_SUFFIX=${CMAKE_SHARED_LIBRARY_SUFFIX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/PackageCheck.cmake)
    set_tests_properties(package_cli PROPERTIES TIMEOUT 120 LABELS gpu)
endif()

add_executable(project_test ProjectTest.cpp)
target_link_libraries(project_test PRIVATE Shockwave::GameFramework)
add_test(NAME project COMMAND project_test ${CMAKE_SOURCE_DIR}/Templates/Blank
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME project_create_cli COMMAND ${CMAKE_COMMAND}
    -DPLAYER=$<TARGET_FILE:player>
    -DTEMP=${CMAKE_CURRENT_BINARY_DIR}/ProjectCreateCLI
    -P ${CMAKE_CURRENT_SOURCE_DIR}/ProjectCreateCheck.cmake)

add_executable(save_game_test SaveGameTest.cpp)
target_link_libraries(save_game_test PRIVATE Shockwave::GameFramework)
shockwave_warnings(save_game_test)
add_test(NAME save_games COMMAND save_game_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(save_games PROPERTIES ENVIRONMENT "SDL_AUDIO_DRIVER=dummy" TIMEOUT 60)

# Circuits (visual scripting): format, compiler checks and circuits running without a window.
add_executable(circuit_test CircuitTest.cpp)
target_link_libraries(circuit_test PRIVATE Shockwave::Circuits)
shockwave_warnings(circuit_test)
add_test(NAME circuits COMMAND circuit_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(model_test ModelTest.cpp)
target_link_libraries(model_test PRIVATE Shockwave::Engine)
add_test(NAME model_import COMMAND model_test ${CMAKE_SOURCE_DIR}
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Explicitly invoked only: needs a real Vulkan GPU/display.
add_executable(model_gpu_smoke ModelGpuSmoke.cpp)
target_link_libraries(model_gpu_smoke PRIVATE Shockwave::Engine)

add_executable(physics_test PhysicsTest.cpp)
target_link_libraries(physics_test PRIVATE Shockwave::GameFramework)
add_test(NAME physics COMMAND physics_test ${CMAKE_SOURCE_DIR}
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(physics PROPERTIES TIMEOUT 60)

add_executable(input_test InputTest.cpp)
target_link_libraries(input_test PRIVATE Shockwave::Input)
shockwave_warnings(input_test)
add_test(NAME input COMMAND input_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(input PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")

add_executable(audio_test AudioTest.cpp)
target_link_libraries(audio_test PRIVATE Shockwave::GameFramework)
shockwave_warnings(audio_test)
add_test(NAME audio COMMAND audio_test ${CMAKE_SOURCE_DIR})
set_tests_properties(audio PROPERTIES ENVIRONMENT "SDL_AUDIO_DRIVER=dummy" TIMEOUT 30)
# Spatial audio twice: with Steam Audio (when this build ships libphonon) and with the
# library hidden, which must fall back to simple panning.
add_executable(spatial_audio_test SpatialAudioTest.cpp)
target_link_libraries(spatial_audio_test PRIVATE Shockwave::Audio)
shockwave_warnings(spatial_audio_test)
if(SHOCKWAVE_STEAM_AUDIO AND steamaudio_library AND EXISTS ${steamaudio_library})
    add_test(NAME spatial_audio COMMAND spatial_audio_test steam)
    set_tests_properties(spatial_audio PROPERTIES ENVIRONMENT "SDL_AUDIO_DRIVER=dummy" TIMEOUT 60)
endif()
add_test(NAME spatial_audio_fallback COMMAND spatial_audio_test simple)
set_tests_properties(spatial_audio_fallback PROPERTIES ENVIRONMENT "SDL_AUDIO_DRIVER=dummy;SHOCKWAVE_PHONON=none" TIMEOUT 60)
find_program(SHOCKWAVE_XVFB_RUN xvfb-run)
if(SHOCKWAVE_XVFB_RUN AND TARGET game_sample)
    add_test(NAME audio_cli_check COMMAND ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:player>
        -DPROJECT=${CMAKE_SOURCE_DIR}/Games/Sample/Sample.swproject
        -DTEMP=${CMAKE_CURRENT_BINARY_DIR}/AudioCLI
        -DXVFB=${SHOCKWAVE_XVFB_RUN}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/AudioCheck.cmake)
    set_tests_properties(audio_cli_check PROPERTIES ENVIRONMENT "SDL_AUDIO_DRIVER=dummy" TIMEOUT 60)
    add_test(NAME save_games_cli COMMAND ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:player>
        -DPROJECT=${CMAKE_SOURCE_DIR}/Games/Sample/Sample.swproject
        -DXVFB=${SHOCKWAVE_XVFB_RUN}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/SaveGameCheck.cmake)
    set_tests_properties(save_games_cli PROPERTIES ENVIRONMENT "SDL_AUDIO_DRIVER=dummy" TIMEOUT 120)
endif()

add_executable(gpu_selection_test GpuSelectionTest.cpp)
target_link_libraries(gpu_selection_test PRIVATE Shockwave::Renderer)
shockwave_warnings(gpu_selection_test)
add_test(NAME gpu_selection COMMAND gpu_selection_test)

add_executable(frame_pacing_test FramePacingTest.cpp)
target_link_libraries(frame_pacing_test PRIVATE Shockwave::Projects)
shockwave_warnings(frame_pacing_test)
add_test(NAME frame_pacing COMMAND frame_pacing_test ${CMAKE_SOURCE_DIR}
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
# It measures real sleeps, which shared macOS CI machines stretch by several milliseconds;
# CI runs tests labelled timing there without failing the build.
set_tests_properties(frame_pacing PROPERTIES LABELS timing)

# Mesh optimisation and texture compression without a GPU (docs/Optimisation.md).
add_executable(optimisation_test OptimisationTest.cpp)
target_link_libraries(optimisation_test PRIVATE Shockwave::Assets Shockwave::Projects)
shockwave_warnings(optimisation_test)
add_test(NAME optimisation COMMAND optimisation_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Plugins: renderer swapping and game code hot reload (modular builds only, where they
# are libraries).
if(SHOCKWAVE_MODULAR)
    foreach(version 1 2)
        add_library(reload_module_v${version} MODULE fixtures/HotReload/ReloadModule.cpp
                    ${PROJECT_SOURCE_DIR}/Source/Runtime/GameFramework/Entry/GameModuleEntry.cpp)
        target_link_libraries(reload_module_v${version} PRIVATE Shockwave::GameFramework)
        target_compile_definitions(reload_module_v${version} PRIVATE RELOAD_VERSION=${version})
        set_target_properties(reload_module_v${version} PROPERTIES PREFIX "" DEBUG_POSTFIX ""
                              LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(reload_module_v${version} PRIVATE -fno-gnu-unique)
        endif()
    endforeach()
    add_executable(hot_reload_test HotReloadTest.cpp)
    target_link_libraries(hot_reload_test PRIVATE Shockwave::GameFramework)
    shockwave_warnings(hot_reload_test)
    add_test(NAME hot_reload COMMAND hot_reload_test $<TARGET_FILE:reload_module_v1> $<TARGET_FILE:reload_module_v2>)

    add_executable(renderer_swap_test RendererSwapTest.cpp)
    target_link_libraries(renderer_swap_test PRIVATE Shockwave::Engine)
    shockwave_warnings(renderer_swap_test)
    shockwave_link_plugins(renderer_swap_test)
    add_test(NAME renderer_swap COMMAND renderer_swap_test)
    set_tests_properties(renderer_swap PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;SDL_AUDIO_DRIVER=dummy"
                         TIMEOUT 60 LABELS gpu)
endif()

# Game UI (RmlUi, FreeType, HarfBuzz) without a GPU: layout, shaping, data binding,
# problems, hot reload and events.
add_executable(game_ui_test GameUITest.cpp)
target_link_libraries(game_ui_test PRIVATE Shockwave::GameUI)
shockwave_warnings(game_ui_test)
add_test(NAME game_ui COMMAND game_ui_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Navigation (Recast & Detour) without a GPU: baking, paths, rebakes, files and agents.
add_executable(navigation_test NavigationTest.cpp)
target_link_libraries(navigation_test PRIVATE Shockwave::GameFramework)
shockwave_warnings(navigation_test)
add_test(NAME navigation COMMAND navigation_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(navigation PROPERTIES TIMEOUT 60)
add_executable(effects_test EffectsTest.cpp)
target_link_libraries(effects_test PRIVATE Shockwave::Effects)
shockwave_warnings(effects_test)
add_test(NAME effects COMMAND effects_test ${CMAKE_SOURCE_DIR} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
# Skeletal animation (ozz-animation) without a GPU: import, play, loop, cross-fade,
# joints and the Animator checks, on the sample's Fox.
add_executable(animation_test AnimationTest.cpp)
target_link_libraries(animation_test PRIVATE Shockwave::Animation)
shockwave_warnings(animation_test)
add_test(NAME animation COMMAND animation_test ${CMAKE_SOURCE_DIR})

if(TARGET game_sample)
add_test(NAME circuits_cli COMMAND ${CMAKE_COMMAND}
    -DPLAYER=$<TARGET_FILE:player>
    -DPROJECT=${CMAKE_SOURCE_DIR}/Games/Sample/Sample.swproject
    -DNODES=${CMAKE_SOURCE_DIR}/docs/CircuitNodes.md
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CircuitCheck.cmake)
set_tests_properties(circuits_cli PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;SDL_AUDIO_DRIVER=dummy" TIMEOUT 60
                     LABELS gpu)
endif()

add_executable(graphics_test GraphicsTest.cpp)
target_link_libraries(graphics_test PRIVATE Shockwave::Engine)
shockwave_warnings(graphics_test)
shockwave_link_plugins(graphics_test)
add_test(NAME graphics COMMAND graphics_test)
set_tests_properties(graphics PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;SDL_AUDIO_DRIVER=dummy" TIMEOUT 30)
if(TARGET game_arena)
    add_test(NAME graphics_cli COMMAND ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:player>
        -DPROJECT=${CMAKE_SOURCE_DIR}/Games/Arena/Arena.swproject
        -DTEMP=${CMAKE_CURRENT_BINARY_DIR}/GraphicsCLI
        -P ${CMAKE_CURRENT_SOURCE_DIR}/GraphicsCheck.cmake)
    set_tests_properties(graphics_cli PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;SDL_AUDIO_DRIVER=dummy" TIMEOUT 60)
endif()

# Networking (Network module): the wire format and its reference doc, the loopback
# transport with fault injection, and packet captures. No Steam, sockets or display.
add_executable(network_wire_test NetworkWireTest.cpp)
target_link_libraries(network_wire_test PRIVATE Shockwave::Network)
shockwave_warnings(network_wire_test)
add_test(NAME network_wire COMMAND network_wire_test ${CMAKE_SOURCE_DIR}/docs/NetworkProtocol.md)
set_tests_properties(network_wire PROPERTIES TIMEOUT 120)

add_executable(network_transport_test NetworkTransportTest.cpp)
target_link_libraries(network_transport_test PRIVATE Shockwave::Network)
shockwave_warnings(network_transport_test)
add_test(NAME network_transport COMMAND network_transport_test)
set_tests_properties(network_transport PROPERTIES TIMEOUT 120)

add_executable(network_capture_test NetworkCaptureTest.cpp)
target_link_libraries(network_capture_test PRIVATE Shockwave::Network)
shockwave_warnings(network_capture_test)
add_test(NAME network_capture COMMAND network_capture_test)
set_tests_properties(network_capture PROPERTIES TIMEOUT 60)

# Batch 2: network settings, manifest, digests, the replication registry and its codec.
add_executable(network_settings_test NetworkSettingsTest.cpp)
target_link_libraries(network_settings_test PRIVATE Shockwave::Multiplayer)
shockwave_warnings(network_settings_test)
add_test(NAME network_settings COMMAND network_settings_test)
set_tests_properties(network_settings PROPERTIES TIMEOUT 60)

# Session integration: real PlaySessions going online over the loopback hub.
add_executable(network_session_test NetworkSessionTest.cpp)
target_link_libraries(network_session_test PRIVATE Shockwave::GameFramework)
shockwave_warnings(network_session_test)
add_test(NAME network_session COMMAND network_session_test)
set_tests_properties(network_session PROPERTIES ENVIRONMENT "SDL_AUDIO_DRIVER=dummy;SDL_VIDEO_DRIVER=dummy" TIMEOUT 300)

# Replication without prediction: baselines, keys, spawns, snapshots, budgets, bad data.
add_executable(network_replication_test NetworkReplicationTest.cpp)
target_link_libraries(network_replication_test PRIVATE Shockwave::Multiplayer)
shockwave_warnings(network_replication_test)
add_test(NAME network_replication COMMAND network_replication_test)
set_tests_properties(network_replication PROPERTIES TIMEOUT 300)

# Prediction and presentation: Physics' network API, the input sampler, the prediction
# net-test on every fault preset.
add_executable(network_prediction_test NetworkPredictionTest.cpp)
target_link_libraries(network_prediction_test PRIVATE Shockwave::GameFramework)
shockwave_warnings(network_prediction_test)
add_test(NAME network_prediction COMMAND network_prediction_test)
set_tests_properties(network_prediction PROPERTIES TIMEOUT 300)

# Batch 8: lag-compensated shots, game events, presentation dedup and network circuits.
add_executable(network_combat_test NetworkCombatTest.cpp)
target_link_libraries(network_combat_test PRIVATE Shockwave::GameFramework)
shockwave_warnings(network_combat_test)
add_test(NAME network_combat COMMAND network_combat_test)
set_tests_properties(network_combat PROPERTIES TIMEOUT 300)

# Delta snapshots, interpolation, rewind history.
add_executable(test_snapshot_delta SnapshotDeltaTest.cpp)
target_link_libraries(test_snapshot_delta PRIVATE Shockwave::Multiplayer)
shockwave_warnings(test_snapshot_delta)
add_test(NAME test_snapshot_delta COMMAND test_snapshot_delta)
set_tests_properties(test_snapshot_delta PROPERTIES TIMEOUT 120)

add_executable(test_rewind_buffer RewindBufferTest.cpp)
target_link_libraries(test_rewind_buffer PRIVATE Shockwave::Multiplayer)
shockwave_warnings(test_rewind_buffer)
add_test(NAME test_rewind_buffer COMMAND test_rewind_buffer)
set_tests_properties(test_rewind_buffer PROPERTIES TIMEOUT 120)

add_executable(test_replication_allocations ReplicationAllocationTest.cpp)
target_link_libraries(test_replication_allocations PRIVATE Shockwave::Multiplayer)
shockwave_warnings(test_replication_allocations)
add_test(NAME test_replication_allocations COMMAND test_replication_allocations)
set_tests_properties(test_replication_allocations PROPERTIES TIMEOUT 120)

# Batch 3: Steam services and the Steam Datagram Relay transport, against MockSteamApi, so
# they run without the Steamworks SDK, a Steam client or an AppID.
add_executable(test_steam_services SteamServicesTest.cpp)
target_link_libraries(test_steam_services PRIVATE Shockwave::SteamServices)
shockwave_warnings(test_steam_services)
if(SHOCKWAVE_STEAMWORKS)
    target_compile_definitions(test_steam_services PRIVATE SHOCKWAVE_TEST_STEAM_SDK=1)
endif()
add_test(NAME test_steam_services COMMAND test_steam_services)
set_tests_properties(test_steam_services PROPERTIES TIMEOUT 60)

# Batch 9: hosting, finding, joining, inviting and leaving (OnlineMatch) over the Steam
# mock with real sessions, and a packet capture of one.
add_executable(online_match_test OnlineMatchTest.cpp)
target_link_libraries(online_match_test PRIVATE Shockwave::Multiplayer Shockwave::SteamServices)
shockwave_warnings(online_match_test)
add_test(NAME online_match COMMAND online_match_test)
set_tests_properties(online_match PROPERTIES TIMEOUT 120)

add_executable(test_sdr_transport_mock SdrTransportMockTest.cpp)
target_link_libraries(test_sdr_transport_mock PRIVATE Shockwave::SteamServices)
shockwave_warnings(test_sdr_transport_mock)
add_test(NAME test_sdr_transport_mock COMMAND test_sdr_transport_mock)
set_tests_properties(test_sdr_transport_mock PROPERTIES TIMEOUT 120)

# The real Steam, by hand (-DSHOCKWAVE_STEAM_TESTS=ON with the SDK): Steam running and
# signed in, a project whose Config/Network.swnetwork has the test AppID, and its
# steam_appid.txt in the working folder. Signs in, waits for relay access, shuts down.
if(SHOCKWAVE_STEAM_TESTS)
    set(SHOCKWAVE_STEAM_TEST_PROJECT "" CACHE FILEPATH "The .swproject steam_status signs in for")
    if(NOT SHOCKWAVE_STEAM_TEST_PROJECT)
        message(FATAL_ERROR "SHOCKWAVE_STEAM_TESTS needs SHOCKWAVE_STEAM_TEST_PROJECT=<file.swproject>")
    endif()
    add_test(NAME steam_status COMMAND player --project ${SHOCKWAVE_STEAM_TEST_PROJECT} --steam-status)
    set_tests_properties(steam_status PROPERTIES TIMEOUT 60)
endif()
