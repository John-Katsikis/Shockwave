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
