# Reactor Run (S07, README.md): the shipping plan's finishable reference game. Every .cpp
# in Source/ is the module; the tests link the same sources without the module's entry.
shockwave_game_module(shippingdemo)

if(BUILD_TESTING)
    set(DEMO_SOURCES Source/DemoComponents.cpp Source/DemoRules.cpp Source/DemoGame.cpp)
    # Objectives, combat, pickups, precedence, saves and frame-rate independence on the
    # real scenes, with real Jolt and input injected at the device boundary.
    add_executable(shipping_demo_test Tests/DemoTest.cpp ${DEMO_SOURCES})
    target_include_directories(shipping_demo_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_link_libraries(shipping_demo_test PRIVATE Shockwave::GameFramework)
    shockwave_warnings(shipping_demo_test)
    add_test(NAME shipping_demo COMMAND shipping_demo_test ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(shipping_demo PROPERTIES ENVIRONMENT "SDL_AUDIO_DRIVER=dummy" TIMEOUT 120)
endif()

if(BUILD_TESTING)
    # The whole game through the player with real input, and save/quit/Continue
    # (Tests/flow/*.json, Tests/DemoFlowCheck.cmake).
    add_test(NAME shipping_demo_flow COMMAND ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:player>
        -DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/ShippingDemo.swproject
        -DFLOWS=${CMAKE_CURRENT_SOURCE_DIR}/Tests/flow
        -DTEMP=${CMAKE_CURRENT_BINARY_DIR}/DemoFlow
        -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/DemoFlowCheck.cmake)
    set_tests_properties(shipping_demo_flow PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;SDL_AUDIO_DRIVER=dummy" TIMEOUT 600)
endif()
