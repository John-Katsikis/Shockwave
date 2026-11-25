# Terrain (W1a) on the GPU, with the graphics fixtures (Scenes/terrain-hills*.swscene):
#   - two --fixed-step runs render the same pixels;
#   - no crack between chunks at different detail shows the sky: the same hills under a
#     magenta sky match below the horizon (only reflections of the sky may move a little),
#     with and without --lod-colors;
#   - the report counts the chunks and the simpler levels drawn, and lod_error_pixels 0
#     draws every chunk at full detail.
cmake_policy(SET CMP0054 NEW)   # quoted strings in if() are never variables
file(MAKE_DIRECTORY "${TEMP}")
function(render scene name)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/${scene}.swscene" --fixed-step --size 320x180 ${ARGN}
        --screenshot "${TEMP}/${name}.png"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "[exit ${result}] ${scene} (${ARGN}) didn't render: ${out}\n${err}")
    endif()
endfunction()
function(diff a b region out_var)
    execute_process(COMMAND "${PLAYER}" --image-diff "${TEMP}/${a}.png" "${TEMP}/${b}.png" --tolerance 255
        --region ${region} --json "${TEMP}/diff.json" RESULT_VARIABLE result OUTPUT_QUIET)
    file(READ "${TEMP}/diff.json" json)
    set(${out_var} "${json}" PARENT_SCOPE)
endfunction()
function(report name)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/terrain-hills.swscene" --fixed-step --size 320x180 ${ARGN} --graphics-report
        --json "${TEMP}/${name}.json" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "[exit ${result}] --graphics-report (${ARGN}) failed: ${out}\n${err}")
    endif()
    file(READ "${TEMP}/${name}.json" json)
    string(JSON terrains GET "${json}" terrain terrains)
    string(JSON chunks GET "${json}" terrain chunks)
    string(JSON reduced GET "${json}" terrain lod_reduced_draws)
    set(terrains "${terrains}" PARENT_SCOPE)
    set(chunks "${chunks}" PARENT_SCOPE)
    set(reduced "${reduced}" PARENT_SCOPE)
endfunction()

render(terrain-hills hills)
render(terrain-hills hills-2)
diff(hills hills-2 "0,0,1,1" json)
string(JSON rmse GET "${json}" rmse)
if(NOT rmse EQUAL 0)
    message(FATAL_ERROR "two --fixed-step runs of the terrain differ (RMSE ${rmse})")
endif()

# Below the horizon (the lower 40% of the picture) is all terrain. A crack would show the
# sky: grey-blue in one, magenta in the other, a difference of over 100 in a channel.
foreach(view "" "--lod-colors")
    set(suffix "")
    if(view)
        set(suffix "-lod")
    endif()
    render(terrain-hills grey${suffix} ${view})
    render(terrain-hills-sky magenta${suffix} ${view})
    diff(grey${suffix} magenta${suffix} "0,0.6,1,0.4" json)
    string(JSON most GET "${json}" max_diff)
    if(most GREATER_EQUAL 40)
        message(FATAL_ERROR "the sky shows through the terrain ${view} (largest difference ${most} below the horizon)")
    endif()
endforeach()
# The level colours tint the picture: chunks at more than one level are in view.
diff(grey grey-lod "0,0.6,1,0.4" json)
string(JSON changed GET "${json}" changed_percent)
if(changed LESS 50)
    message(FATAL_ERROR "--lod-colors changed only ${changed}% of the terrain")
endif()

report(report)
if(NOT terrains EQUAL 1 OR NOT chunks EQUAL 16)
    message(FATAL_ERROR "the report counts ${terrains} terrains and ${chunks} chunks, not 1 and 16")
endif()
if(reduced LESS 1)
    message(FATAL_ERROR "no terrain chunk drew a simpler level (${reduced})")
endif()
report(full --graphics-set lod_error_pixels=0)
if(NOT reduced EQUAL 0)
    message(FATAL_ERROR "lod_error_pixels 0 still drew ${reduced} simpler levels")
endif()
message(STATUS "terrain: deterministic, no cracks, ${chunks} chunks, levels of detail as set")
