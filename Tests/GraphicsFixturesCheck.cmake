# Renders each graphics fixture scene once (Tests/fixtures/graphics) and checks the result
# isn't empty, then renders the moving scene twice with --fixed-step and requires identical
# pixels. Comparing against the recorded baseline is Tools/capture.py --compare: baselines
# are only meaningful on the driver that recorded them.
file(MAKE_DIRECTORY "${TEMP}")
file(GLOB scenes "${FIXTURE}/Scenes/*.swscene")
foreach(scene ${scenes})
    get_filename_component(name "${scene}" NAME_WE)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics --scene "${scene}"
        --size 320x180 --fixed-step --frames 10 --screenshot "${TEMP}/${name}.png"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name} didn't render: ${out}\n${err}")
    endif()
    execute_process(COMMAND "${PLAYER}" --image-stats "${TEMP}/${name}.png" RESULT_VARIABLE result OUTPUT_VARIABLE stats)
    string(REGEX MATCH "\"mean_luma\": ([0-9.]+)" _ "${stats}")
    if(NOT result EQUAL 0 OR CMAKE_MATCH_1 LESS 10)
        message(FATAL_ERROR "${name} rendered (nearly) black: ${stats}")
    endif()
endforeach()
foreach(run 1 2)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/motion.swscene" --camera "Orbit camera" --size 320x180 --fixed-step --frames 40
        --screenshot "${TEMP}/motion-${run}.png" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "motion run ${run} failed: ${out}\n${err}")
    endif()
endforeach()
execute_process(COMMAND "${PLAYER}" --image-diff "${TEMP}/motion-1.png" "${TEMP}/motion-2.png" --tolerance 0
    RESULT_VARIABLE result OUTPUT_VARIABLE out)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "--fixed-step frames differ between runs: ${out}")
endif()
execute_process(COMMAND "${PLAYER}" --image-diff "${TEMP}/motion.png" "${TEMP}/motion-1.png" --tolerance 0
    RESULT_VARIABLE result OUTPUT_VARIABLE out)
if(result EQUAL 0)
    message(FATAL_ERROR "frame 10 and frame 40 of the moving scene are identical: nothing moved")
endif()
file(REMOVE_RECURSE "${TEMP}")
