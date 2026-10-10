add_executable(ecs_test EcsTest.cpp)
target_link_libraries(ecs_test PRIVATE shockwave)
if(MSVC)
    target_compile_options(ecs_test PRIVATE /W4)
else()
    target_compile_options(ecs_test PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME ecs COMMAND ecs_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(ui_theme_test UiThemeTest.cpp)
target_link_libraries(ui_theme_test PRIVATE shockwave)
add_test(NAME ui_theme COMMAND ui_theme_test ${CMAKE_SOURCE_DIR}
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(project_test ProjectTest.cpp)
target_link_libraries(project_test PRIVATE shockwave)
add_test(NAME project COMMAND project_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(model_test ModelTest.cpp)
target_link_libraries(model_test PRIVATE shockwave)
add_test(NAME model_import COMMAND model_test ${CMAKE_SOURCE_DIR}
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Explicitly invoked only: needs a real Vulkan GPU/display.
add_executable(model_gpu_smoke ModelGpuSmoke.cpp)
target_link_libraries(model_gpu_smoke PRIVATE shockwave)

add_executable(physics_test PhysicsTest.cpp)
target_link_libraries(physics_test PRIVATE shockwave)
add_test(NAME physics COMMAND physics_test ${CMAKE_SOURCE_DIR}
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(physics PROPERTIES TIMEOUT 60)
