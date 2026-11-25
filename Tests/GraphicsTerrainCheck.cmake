# Terrain (W1a) on the GPU, with the graphics fixtures (Scenes/terrain-hills*.swscene):
#   - two --fixed-step runs render the same pixels;
#   - no crack between chunks at different detail shows the sky: the same hills under a
#     magenta sky match below the horizon (only reflections of the sky may move a little),
#     with and without --lod-colors;
#   - the report counts the chunks and the simpler levels drawn, and lod_error_pixels 0
#     draws every chunk at full detail.
# Painted layers (W1b, Scenes/terrain-layers.swscene, seen from above):
#   - the four quadrants show four distinct colours, each its layer's;
#   - terrain_quality low differs from high only in the disc where all four layers mix;
#   - the report counts the painted draws and the layers they sample.
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

# ---- W1b: painted layers ----
render(terrain-layers layers-high --graphics-set terrain_quality=high)
render(terrain-layers layers-high-2 --graphics-set terrain_quality=high)
diff(layers-high layers-high-2 "0,0,1,1" json)
string(JSON rmse GET "${json}" rmse)
if(NOT rmse EQUAL 0)
    message(FATAL_ERROR "two --fixed-step runs of the painted terrain differ (RMSE ${rmse})")
endif()
# Quadrants (screen top is the terrain's low Z): red, green, blue, yellow. Each mean's
# strongest channels must be its layer's, clearly.
function(quadrant region name expect)
    execute_process(COMMAND "${PLAYER}" --image-stats "${TEMP}/layers-high.png" --region ${region}
        --json "${TEMP}/stats.json" OUTPUT_QUIET)
    file(READ "${TEMP}/stats.json" json)
    string(JSON r GET "${json}" mean_rgb 0)
    string(JSON g GET "${json}" mean_rgb 1)
    string(JSON b GET "${json}" mean_rgb 2)
    set(ok FALSE)
    if(expect STREQUAL "red" AND r GREATER 200 AND g LESS 140 AND b LESS 140)
        set(ok TRUE)
    elseif(expect STREQUAL "green" AND g GREATER 190 AND r LESS 170 AND b LESS 170)
        set(ok TRUE)
    elseif(expect STREQUAL "blue" AND b GREATER 190 AND r LESS 140 AND g LESS 150)
        set(ok TRUE)
    elseif(expect STREQUAL "yellow" AND r GREATER 190 AND g GREATER 190 AND b LESS 170)
        set(ok TRUE)
    endif()
    if(NOT ok)
        message(FATAL_ERROR "the ${name} quadrant isn't ${expect}: mean ${r} ${g} ${b}")
    endif()
endfunction()
quadrant("0.33,0.2,0.1,0.18" "low X, low Z" red)
quadrant("0.57,0.2,0.1,0.18" "high X, low Z" green)
quadrant("0.33,0.62,0.1,0.18" "low X, high Z" blue)
quadrant("0.57,0.62,0.1,0.18" "high X, high Z" yellow)

# terrain_quality low: the same wherever at most two layers mix (everything but the centre),
# different in the centre's four-layer disc.
render(terrain-layers layers-low --graphics-set terrain_quality=low)
foreach(region "0,0,1,0.36" "0,0.64,1,0.36" "0,0,0.42,1" "0.58,0,0.42,1")
    diff(layers-high layers-low "${region}" json)
    string(JSON most GET "${json}" max_diff)
    if(most GREATER 2)
        message(FATAL_ERROR "terrain_quality low changed pixels where at most two layers mix (region ${region}, largest difference ${most})")
    endif()
endforeach()
diff(layers-high layers-low "0.42,0.36,0.16,0.28" json)
string(JSON most GET "${json}" max_diff)
if(most LESS 10)
    message(FATAL_ERROR "terrain_quality low didn't change the disc where four layers mix (largest difference ${most})")
endif()

execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
    --scene "${FIXTURE}/Scenes/terrain-layers.swscene" --fixed-step --size 320x180 --graphics-report
    --json "${TEMP}/layers-report.json" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "[exit ${result}] --graphics-report of the painted terrain failed: ${out}\n${err}")
endif()
file(READ "${TEMP}/layers-report.json" json)
string(JSON painted GET "${json}" terrain painted_draws)
string(JSON sampled GET "${json}" terrain layers_sampled)
if(NOT painted EQUAL 1 OR NOT sampled EQUAL 4)
    message(FATAL_ERROR "the report counts ${painted} painted draws sampling ${sampled} layers, not 1 and 4")
endif()
message(STATUS "terrain: deterministic, no cracks, ${chunks} chunks, levels of detail as set; four painted layers, low differs only where three or more mix")
