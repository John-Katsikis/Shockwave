# Motion vectors and TAA (G09, G10) on the GPU, with the graphics fixtures:
#   - the motion view reads exactly "still" (127, 127, 255) on the fixed camera's floor, moves
#     on the bending skinned strip (its previous pose), and moves on the floor once the
#     camera does;
#   - the reactive view is bright on a transparent panel and 0 on opaque floor;
#   - TAA brings thin geometry closer to a 2x supersampled image than no anti-aliasing,
#     and keeps the brightness;
#   - TAA with --fixed-step renders the same pixels twice;
#   - the report says TAA runs, history carries over and every draw has a previous pose,
#     and with anti_aliasing none there's no temporal pass and no temporal memory.
cmake_policy(SET CMP0054 NEW)   # quoted strings in if() are never variables
file(MAKE_DIRECTORY "${TEMP}")
function(render scene name)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/${scene}.swscene" --fixed-step ${ARGN} --screenshot "${TEMP}/${name}.png"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${scene} (${ARGN}) didn't render: ${out}\n${err}")
    endif()
endfunction()
# mean_rgb of a region of a screenshot, as a list.
function(region_mean name region out_var)
    execute_process(COMMAND "${PLAYER}" --image-stats "${TEMP}/${name}.png" --region ${region}
        --json "${TEMP}/stats.json" RESULT_VARIABLE result OUTPUT_QUIET)
    file(READ "${TEMP}/stats.json" json)
    string(JSON r GET "${json}" mean_rgb 0)
    string(JSON g GET "${json}" mean_rgb 1)
    string(JSON b GET "${json}" mean_rgb 2)
    set(${out_var} "${r};${g};${b}" PARENT_SCOPE)
endfunction()
function(diff a b region out_var)
    execute_process(COMMAND "${PLAYER}" --image-diff "${TEMP}/${a}.png" "${TEMP}/${b}.png" --tolerance 255
        --region ${region} --json "${TEMP}/diff.json" RESULT_VARIABLE result OUTPUT_QUIET)
    file(READ "${TEMP}/diff.json" json)
    set(${out_var} "${json}" PARENT_SCOPE)
endfunction()
# value in [lo, hi]
function(require_between what value lo hi)
    if(value LESS lo OR value GREATER hi)
        message(FATAL_ERROR "${what}: ${value} isn't between ${lo} and ${hi}")
    endif()
endfunction()

# ---- Motion view: 0.5 + pixels / 32 per axis, so still is 127 (0.5 encoded), blue 255 ----
render(motion motion-still --camera Fixed --size 320x180 --frames 10 --view motion)
region_mean(motion-still "0,0.75,1,0.2" floor)
list(GET floor 0 r)
list(GET floor 1 g)
list(GET floor 2 b)
require_between("still floor, red (x motion)" ${r} 126.5 127.5)
require_between("still floor, green (y motion)" ${g} 126.5 127.5)
require_between("still floor, blue (valid)" ${b} 254.5 255)
# The skinned strip bends: its top moves left (red under 127) with a known previous pose.
region_mean(motion-still "0.58,0.17,0.02,0.05" strip)
list(GET strip 0 r)
require_between("bending strip, red (x motion)" ${r} 0 125)
# The orbiting camera moves the still floor across the screen.
render(motion motion-orbit --camera "Orbit camera" --size 320x180 --frames 10 --view motion)
region_mean(motion-orbit "0,0.75,1,0.2" floor)
list(GET floor 0 r)
if(r GREATER 126.5 AND r LESS 127.5)
    message(FATAL_ERROR "the orbiting camera's floor reads as still in the motion view: ${floor}")
endif()

# ---- Reactive view: the blue panel's coverage, nothing on opaque floor ----
render(transparency reactive --size 320x180 --frames 10 --view reactive)
region_mean(reactive "0.15,0.35,0.1,0.15" panel)
list(GET panel 0 r)
require_between("transparent panel in the reactive mask" ${r} 50 255)
region_mean(reactive "0,0.85,1,0.15" floor)
list(GET floor 0 r)
require_between("opaque floor in the reactive mask" ${r} 0 0.5)

# ---- TAA against a supersampled image: the geometry above the floor (the floor's texture
# shimmer depends on mip selection, which a 2x render changes) ----
render(thin-geometry taa-none --size 640x360 --frames 10 --anti-aliasing none)
render(thin-geometry taa-on --size 640x360 --frames 10 --anti-aliasing taa)
render(thin-geometry taa-reference --size 640x360 --frames 10 --anti-aliasing none --render-scale 2)
diff(taa-reference taa-none "0,0,1,0.42" json)
string(JSON rmse_none GET "${json}" rmse)
diff(taa-reference taa-on "0,0,1,0.42" json)
string(JSON rmse_taa GET "${json}" rmse)
if(NOT rmse_taa LESS rmse_none)
    message(FATAL_ERROR "TAA didn't bring thin-geometry closer to the supersampled image: ${rmse_taa} vs ${rmse_none}")
endif()
diff(taa-none taa-on "0,0,1,1" json)
string(JSON luma_none GET "${json}" reference mean_luma)
string(JSON luma_taa GET "${json}" candidate mean_luma)
string(REGEX REPLACE "\\..*" "" none_int "${luma_none}")
string(REGEX REPLACE "\\..*" "" taa_int "${luma_taa}")
math(EXPR luma_gap "${taa_int} - ${none_int}")
if(luma_gap GREATER 2 OR luma_gap LESS -2)
    message(FATAL_ERROR "TAA changed the mean brightness: ${luma_none} -> ${luma_taa}")
endif()

# ---- Deterministic: the moving scene twice, with history and jitter, the same pixels ----
foreach(run 1 2)
    render(motion taa-motion-${run} --camera "Orbit camera" --size 320x180 --frames 30 --anti-aliasing taa)
endforeach()
diff(taa-motion-1 taa-motion-2 "0,0,1,1" json)
string(JSON rmse GET "${json}" rmse)
if(NOT rmse EQUAL 0)
    message(FATAL_ERROR "TAA with --fixed-step differs between runs (rmse ${rmse})")
endif()

# ---- Reports: on, and off ----
function(report name)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/motion.swscene" --size 320x180 ${ARGN} --graphics-report --json "${TEMP}/${name}.json"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "--graphics-report (${ARGN}) failed: ${out}\n${err}")
    endif()
endfunction()
report(taa-report --anti-aliasing taa)
file(READ "${TEMP}/taa-report.json" json)
string(JSON graph GET "${json}" frame_graph)
string(JSON bytes GET "${json}" memory categories "temporal history" bytes)
string(JSON taa_ran GET "${json}" temporal taa)
string(JSON reset GET "${json}" temporal reset)
string(JSON history_draws GET "${json}" temporal history_draws)
string(JSON new_draws GET "${json}" temporal new_draws)
string(JSON draws GET "${json}" draws)
if(NOT graph MATCHES "taa resolve copy" OR NOT graph MATCHES "reactive mask" OR NOT graph MATCHES "motion vectors"
   OR bytes EQUAL 0 OR NOT taa_ran)
    message(FATAL_ERROR "TAA on is missing a pass or its memory (${bytes} bytes):\n${graph}")
endif()
if(NOT reset STREQUAL "none" OR NOT history_draws EQUAL draws OR NOT new_draws EQUAL 0)
    message(FATAL_ERROR "TAA history didn't carry over: reset ${reset}, ${history_draws} of ${draws} draws with a "
                        "previous pose, ${new_draws} new")
endif()
string(JSON count LENGTH "${json}" settings)
math(EXPR last "${count} - 1")
set(effective "")
foreach(i RANGE ${last})
    string(JSON name GET "${json}" settings ${i} name)
    if(name STREQUAL "anti_aliasing")
        string(JSON effective GET "${json}" settings ${i} effective)
    endif()
endforeach()
if(NOT effective STREQUAL "taa")
    message(FATAL_ERROR "anti_aliasing taa reports as running '${effective}'")
endif()
report(off-report --anti-aliasing none --graphics-set ambient_occlusion=off)
file(READ "${TEMP}/off-report.json" json)
string(JSON graph GET "${json}" frame_graph)
string(JSON bytes GET "${json}" memory categories "temporal history" bytes)
string(JSON motion GET "${json}" temporal motion)
if(graph MATCHES "taa" OR graph MATCHES "reactive" OR graph MATCHES "motion vectors" OR graph MATCHES "prepass"
   OR NOT bytes EQUAL 0 OR motion)
    message(FATAL_ERROR "anti_aliasing none still costs temporal passes or memory (${bytes} bytes):\n${graph}")
endif()
