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

# Material bindings (G02): materials.swscene reads its quads through KHR_texture_transform,
# TEXCOORD_1 and sampler modes; materials-expected.swscene has the same quads with all of
# it baked into plain UV0. Albedo (unlit), normal maps (normals) and the lit image with
# its alpha-tested shadows must match. Nearest sampling may round a texel edge a pixel
# either way (about 1.5% of the image, all in the clamp/mirror quad), hence the
# tolerance; one wrong quad changes over 3% of the image by far more.
foreach(view unlit normals lit)
    foreach(scene materials materials-expected)
        execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
            --scene "${FIXTURE}/Scenes/${scene}.swscene" --size 640x360 --fixed-step --frames 10 --view ${view}
            --screenshot "${TEMP}/${scene}-${view}.png" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "${scene} (${view}) didn't render: ${out}\n${err}")
        endif()
    endforeach()
    image_diff(materials-expected-${view} materials-${view} json)
    string(JSON rmse GET "${json}" rmse)
    string(JSON changed GET "${json}" changed_percent)
    if(rmse GREATER 1.5 OR changed GREATER 2.5)
        message(FATAL_ERROR "material bindings (${view} view) don't match the baked UVs: rmse ${rmse}, ${changed}% changed")
    endif()
endforeach()

# Material instances (A02): material-instances.swscene draws every materials.glb part
# through a .swmaterial that only names its source material. Resolved through the same
# path as the model's own materials, it must match bit for bit in every view.
foreach(view unlit normals lit)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/material-instances.swscene" --size 640x360 --fixed-step --frames 10 --view ${view}
        --screenshot "${TEMP}/material-instances-${view}.png" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "material-instances (${view}) didn't render: ${out}\n${err}")
    endif()
    image_diff(materials-${view} material-instances-${view} json)
    string(JSON rmse GET "${json}" rmse)
    if(NOT rmse EQUAL 0)
        message(FATAL_ERROR "material instances (${view} view) don't draw exactly like their source: rmse ${rmse}")
    endif()
endforeach()

# Material quality (A02): low leaves out the detail layer, so the detailed cube changes,
# and only a little of the image does.
foreach(quality high low)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/material-layers.swscene" --size 640x360 --fixed-step --frames 10
        --graphics-set material_quality=${quality} --screenshot "${TEMP}/layers-${quality}.png"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "material-layers at material_quality ${quality} didn't render: ${out}\n${err}")
    endif()
endforeach()
image_diff(layers-high layers-low json)
string(JSON rmse GET "${json}" rmse)
string(JSON changed GET "${json}" changed_percent)
if(rmse EQUAL 0 OR changed GREATER 10)
    message(FATAL_ERROR "material_quality low should drop only the detail layer: rmse ${rmse}, ${changed}% changed")
endif()

# Texture quality (A01): low keeps a fraction of each large material texture on the GPU,
# the image changes (softer: the fine floor checker averages to grey up close), and the
# report says so. That the averaging is in linear light is TextureQualityTest's job.
foreach(quality ultra low)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/transparency.swscene" --size 320x180 --graphics-set texture_quality=${quality}
        --graphics-report --json "${TEMP}/quality-${quality}.json"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "--graphics-report at texture_quality ${quality} failed: ${out}\n${err}")
    endif()
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/transparency.swscene" --size 640x360 --fixed-step --frames 10
        --graphics-set texture_quality=${quality} --screenshot "${TEMP}/quality-${quality}.png"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "transparency at texture_quality ${quality} didn't render: ${out}\n${err}")
    endif()
endforeach()
file(READ "${TEMP}/quality-ultra.json" ultra)
file(READ "${TEMP}/quality-low.json" low)
string(JSON ultra_textures GET "${ultra}" memory categories textures bytes)
string(JSON low_textures GET "${low}" memory categories textures bytes)
string(JSON reduced GET "${low}" texture_residency reduced)
string(JSON pending GET "${low}" texture_residency pending)
string(JSON resident GET "${low}" texture_residency resident_bytes)
string(JSON full GET "${low}" texture_residency full_bytes)
string(JSON ultra_reduced GET "${ultra}" texture_residency reduced)
math(EXPR quarter "${full} / 4")
if(reduced EQUAL 0 OR NOT pending EQUAL 0 OR NOT resident LESS quarter OR NOT low_textures LESS ultra_textures
   OR NOT ultra_reduced EQUAL 0)
    message(FATAL_ERROR "texture_quality low didn't shrink material textures: ${reduced} reduced, ${pending} pending, "
                        "${resident} of ${full} bytes; textures ${low_textures} vs ${ultra_textures} at ultra")
endif()
image_diff(quality-ultra quality-low json)
string(JSON rmse GET "${json}" rmse)
if(rmse EQUAL 0)
    message(FATAL_ERROR "texture_quality low rendered exactly like ultra: the smaller images weren't used")
endif()
file(REMOVE_RECURSE "${TEMP}")
