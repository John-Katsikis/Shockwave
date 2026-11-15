# Every test here needs a TIMEOUT and a label (checked at the end of this folder).
cmake_language(DEFER CALL shockwave_check_tests)
shockwave_game_module(renderlab)
# Offline, source-available conversion/inspection tool. No new dependencies.
add_executable(renderlab_import Tools/ImportModel.cpp)
target_link_libraries(renderlab_import PRIVATE Shockwave::Assets)
shockwave_warnings(renderlab_import)

if(BUILD_TESTING)
    add_test(NAME renderlab_import COMMAND ${CMAKE_COMMAND}
        -DIMPORTER=$<TARGET_FILE:renderlab_import>
        -DTEMP=${CMAKE_CURRENT_BINARY_DIR}/ImportTest
        -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ImportCheck.cmake)
    set_tests_properties(renderlab_import PROPERTIES TIMEOUT 120 LABELS integration)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Content/Library/gun_01/model.gltf")
        add_test(NAME renderlab_check COMMAND player --project ${CMAKE_CURRENT_SOURCE_DIR}/RenderLab.swproject --check)
        set_tests_properties(renderlab_check PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;SDL_AUDIO_DRIVER=dummy" TIMEOUT 120 LABELS integration)
    endif()
endif()
