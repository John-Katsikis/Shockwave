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
