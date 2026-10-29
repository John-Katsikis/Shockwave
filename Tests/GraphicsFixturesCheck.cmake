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
# --graphics-report: the capabilities, settings and memory report, and that every draw in
# the fixtures carries a stable instance key (temporal effects depend on it).
execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
    --scene "${FIXTURE}/Scenes/motion.swscene" --size 320x180 --graphics-report --json "${TEMP}/report.json"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "--graphics-report failed: ${out}\n${err}")
endif()
file(READ "${TEMP}/report.json" report)
string(JSON gpu_bytes GET "${report}" memory gpu_bytes)
string(JSON unkeyed GET "${report}" unkeyed_draws)
string(JSON draws GET "${report}" draws)
string(JSON settings LENGTH "${report}" settings)
if(gpu_bytes EQUAL 0 OR draws EQUAL 0 OR settings EQUAL 0)
    message(FATAL_ERROR "--graphics-report is missing memory, draws or settings: ${report}")
endif()
if(NOT unkeyed EQUAL 0)
    message(FATAL_ERROR "${unkeyed} of ${draws} draws have no RenderInstanceKey")
endif()
# FXAA (G01): against a 2x supersampled reference it must bring the image closer, touch
# only edges (flat areas are left bit for bit), and keep the overall brightness.
function(render_thin name)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/thin-geometry.swscene" --size 640x360 --fixed-step --frames 10 ${ARGN}
        --screenshot "${TEMP}/${name}.png" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "thin-geometry (${ARGN}) didn't render: ${out}\n${err}")
    endif()
endfunction()
function(image_diff a b out_var)
    execute_process(COMMAND "${PLAYER}" --image-diff "${TEMP}/${a}.png" "${TEMP}/${b}.png" --tolerance 255
        --json "${TEMP}/diff.json" RESULT_VARIABLE result OUTPUT_QUIET)
    file(READ "${TEMP}/diff.json" json)
    set(${out_var} "${json}" PARENT_SCOPE)
endfunction()
render_thin(aa-none --anti-aliasing none)
render_thin(aa-fxaa --anti-aliasing fxaa)
render_thin(aa-default)
render_thin(aa-reference --anti-aliasing none --render-scale 2)
image_diff(aa-none aa-default json)
string(JSON rmse GET "${json}" rmse)
if(NOT rmse EQUAL 0)
    message(FATAL_ERROR "anti_aliasing none differs from the default (rmse ${rmse}): none must be the old path")
endif()
image_diff(aa-reference aa-none json)
string(JSON rmse_none GET "${json}" rmse)
image_diff(aa-reference aa-fxaa json)
string(JSON rmse_fxaa GET "${json}" rmse)
if(NOT rmse_fxaa LESS rmse_none)
    message(FATAL_ERROR "FXAA didn't bring thin-geometry closer to the supersampled image: ${rmse_fxaa} vs ${rmse_none}")
endif()
image_diff(aa-none aa-fxaa json)
string(JSON changed GET "${json}" changed_percent)
string(JSON luma_none GET "${json}" reference mean_luma)
string(JSON luma_fxaa GET "${json}" candidate mean_luma)
if(changed GREATER 10)
    message(FATAL_ERROR "FXAA changed ${changed}% of thin-geometry: it should only touch edges")
endif()
if(luma_fxaa GREATER luma_none)
    set(luma_hi ${luma_fxaa})
    set(luma_lo ${luma_none})
else()
    set(luma_hi ${luma_none})
    set(luma_lo ${luma_fxaa})
endif()
string(REGEX REPLACE "\\..*" "" hi_int "${luma_hi}")
string(REGEX REPLACE "\\..*" "" lo_int "${luma_lo}")
math(EXPR luma_gap "${hi_int} - ${lo_int}")
if(luma_gap GREATER 1)
    message(FATAL_ERROR "FXAA changed the mean brightness: ${luma_none} -> ${luma_fxaa}")
endif()

# Off paths (A00): a feature that's off has no pass in the frame graph and no GPU memory.
execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
    --scene "${FIXTURE}/Scenes/thin-geometry.swscene" --size 320x180 --anti-aliasing none
    --graphics-set bloom_quality=off --graphics-report --json "${TEMP}/off.json"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "--graphics-report with bloom and anti-aliasing off failed: ${out}\n${err}")
endif()
file(READ "${TEMP}/off.json" report)
string(JSON graph GET "${report}" frame_graph)
string(JSON bloom_bytes GET "${report}" memory categories bloom bytes)
string(JSON aa_bytes GET "${report}" memory categories anti-aliasing bytes)
if(graph MATCHES "bloom" OR graph MATCHES "fxaa" OR NOT bloom_bytes EQUAL 0 OR NOT aa_bytes EQUAL 0)
    message(FATAL_ERROR "bloom/anti-aliasing off still costs passes or memory (bloom ${bloom_bytes}, aa ${aa_bytes}):\n${graph}")
endif()
foreach(tier low medium high)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/thin-geometry.swscene" --size 320x180 --anti-aliasing fxaa
        --graphics-set bloom_quality=${tier} --graphics-report --json "${TEMP}/on.json"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    file(READ "${TEMP}/on.json" report)
    string(JSON graph GET "${report}" frame_graph)
    string(JSON bloom_bytes GET "${report}" memory categories bloom bytes)
    string(JSON aa_bytes GET "${report}" memory categories anti-aliasing bytes)
    if(NOT result EQUAL 0 OR NOT graph MATCHES "fxaa" OR bloom_bytes EQUAL 0 OR aa_bytes EQUAL 0)
        message(FATAL_ERROR "bloom ${tier} with FXAA is missing a pass or its memory:\n${graph}")
    endif()
endforeach()
file(REMOVE_RECURSE "${TEMP}")
