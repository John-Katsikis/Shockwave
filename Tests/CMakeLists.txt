add_executable(ecs_test EcsTest.cpp)
target_link_libraries(ecs_test PRIVATE Shockwave::Physics)
shockwave_warnings(ecs_test)
add_test(NAME ecs COMMAND ecs_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(ui_theme_test UiThemeTest.cpp)
target_link_libraries(ui_theme_test PRIVATE Shockwave::UI)
add_test(NAME ui_theme COMMAND ui_theme_test ${CMAKE_SOURCE_DIR}
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(project_test ProjectTest.cpp)
target_link_libraries(project_test PRIVATE Shockwave::GameFramework)
add_test(NAME project COMMAND project_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

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
find_program(SHOCKWAVE_XVFB_RUN xvfb-run)
if(SHOCKWAVE_XVFB_RUN)
    add_test(NAME audio_cli_check COMMAND ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:player>
        -DPROJECT=${CMAKE_SOURCE_DIR}/Games/Sample/Sample.swproject
        -DTEMP=${CMAKE_CURRENT_BINARY_DIR}/AudioCLI
        -DXVFB=${SHOCKWAVE_XVFB_RUN}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/AudioCheck.cmake)
    set_tests_properties(audio_cli_check PROPERTIES ENVIRONMENT "SDL_AUDIO_DRIVER=dummy" TIMEOUT 60)
endif()

add_executable(frame_pacing_test FramePacingTest.cpp)
target_link_libraries(frame_pacing_test PRIVATE Shockwave::Projects)
shockwave_warnings(frame_pacing_test)
add_test(NAME frame_pacing COMMAND frame_pacing_test ${CMAKE_SOURCE_DIR}
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

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
                         TIMEOUT 60)
endif()
