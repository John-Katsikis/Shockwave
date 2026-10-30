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
# Surface prepass and ambient occlusion (G03 + G04), on ambient-occlusion.swscene.
function(render_ao name scene)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/${scene}.swscene" --size 640x360 --fixed-step ${ARGN}
        --screenshot "${TEMP}/${name}.png" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${scene} (${ARGN}) didn't render: ${out}\n${err}")
    endif()
endfunction()
function(region_luma image region out_var)
    execute_process(COMMAND "${PLAYER}" --image-stats "${TEMP}/${image}.png" --region ${region}
        RESULT_VARIABLE result OUTPUT_VARIABLE stats)
    string(REGEX MATCH "\"mean_luma\": ([0-9.]+)" _ "${stats}")
    if(NOT result EQUAL 0 OR "${CMAKE_MATCH_1}" STREQUAL "")
        message(FATAL_ERROR "--image-stats ${image} --region ${region} failed: ${stats}")
    endif()
    set(${out_var} ${CMAKE_MATCH_1} PARENT_SCOPE)
endfunction()
function(expect_same a b what)
    image_diff(${a} ${b} json)
    string(JSON rmse GET "${json}" rmse)
    if(NOT rmse EQUAL 0)
        message(FATAL_ERROR "${what} (rmse ${rmse})")
    endif()
endfunction()
render_ao(ao-view ambient-occlusion --frames 10 --view ao --graphics-set ambient_occlusion=high)
# Sky and open floor are white; the box's contact with the floor is darker; the far wall
# right beside the thin pole 7 m in front of it isn't (no halo).
region_luma(ao-view 0.5,0,0.45,0.08 sky)
region_luma(ao-view 0.6,0.75,0.35,0.2 floor)
region_luma(ao-view 0.5,0.55,0.1,0.05 contact)
region_luma(ao-view 0.79,0.25,0.025,0.12 beside_pole)
if(sky LESS 254.5 OR floor LESS 250 OR NOT contact LESS 248 OR beside_pole LESS 250)
    message(FATAL_ERROR "ambient occlusion view: sky ${sky}, open floor ${floor} (both near 255), "
                        "box contact ${contact} (darker), beside the pole ${beside_pole} (near 255)")
endif()
# A still camera sees still occlusion: the sample rotations are fixed, not random per frame.
render_ao(ao-view-40 ambient-occlusion --frames 40 --view ao --graphics-set ambient_occlusion=high)
expect_same(ao-view ao-view-40 "ambient occlusion flickers: frames 10 and 40 of a still scene differ")
# Render scale changes the resolution it's worked out at, not what it finds.
render_ao(ao-view-half ambient-occlusion --frames 10 --view ao --graphics-set ambient_occlusion=high --render-scale 0.5)
image_diff(ao-view ao-view-half json)
string(JSON rmse GET "${json}" rmse)
if(rmse GREATER 4)
    message(FATAL_ERROR "ambient occlusion at render scale 0.5 differs from 1 by rmse ${rmse}")
endif()
# Off is the old frame exactly, and on with nothing to do is too: ao_intensity 0 runs the
# prepass and the EQUAL-depth shading (alpha test and all) and must change no pixel, and so
# must ambient occlusion on a scene without ambient light (direct light is never occluded).
render_ao(ao-lit-off ambient-occlusion --frames 10 --graphics-set ambient_occlusion=off)
render_ao(ao-lit-high ambient-occlusion --frames 10 --graphics-set ambient_occlusion=high)
render_ao(ao-zero-off ambient-occlusion-zero --frames 10 --graphics-set ambient_occlusion=off)
render_ao(ao-zero-high ambient-occlusion-zero --frames 10 --graphics-set ambient_occlusion=high)
render_ao(ao-direct-off ambient-occlusion-direct --frames 10 --graphics-set ambient_occlusion=off)
render_ao(ao-direct-high ambient-occlusion-direct --frames 10 --graphics-set ambient_occlusion=high)
render_ao(ao-default ambient-occlusion --frames 10)
expect_same(ao-lit-off ao-default "ambient_occlusion off differs from the default: off must be the old path")
expect_same(ao-zero-off ao-zero-high "ao_intensity 0 with ambient occlusion on changed the image")
expect_same(ao-direct-off ao-direct-high "ambient occlusion changed a scene lit only directly")
expect_same(ao-lit-off ao-zero-off "ao_intensity changed the image with ambient occlusion off")
image_diff(ao-lit-off ao-lit-high json)
string(JSON rmse GET "${json}" rmse)
if(rmse EQUAL 0)
    message(FATAL_ERROR "ambient occlusion high didn't change the lit image")
endif()
# The glowing block in the corner is black and emissive: occlusion never dims emission.
region_luma(ao-lit-off 0.41,0.445,0.03,0.05 glow_off)
region_luma(ao-lit-high 0.41,0.445,0.03,0.05 glow_high)
if(NOT glow_off EQUAL glow_high)
    message(FATAL_ERROR "ambient occlusion dimmed the emissive block: ${glow_off} -> ${glow_high}")
endif()
# Off has no prepass, no passes and no memory; on has all three.
foreach(ao off medium)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/ambient-occlusion.swscene" --size 320x180 --graphics-set ambient_occlusion=${ao}
        --graphics-report --json "${TEMP}/ao-${ao}.json" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "--graphics-report with ambient_occlusion ${ao} failed: ${out}\n${err}")
    endif()
    file(READ "${TEMP}/ao-${ao}.json" report)
    string(JSON graph GET "${report}" frame_graph)
    string(JSON surface_bytes GET "${report}" memory categories "surface buffers" bytes)
    string(JSON ao_bytes GET "${report}" memory categories "ambient occlusion" bytes)
    if(ao STREQUAL "off")
        if(graph MATCHES "surface prepass" OR graph MATCHES "ambient occlusion" OR NOT surface_bytes EQUAL 0
           OR NOT ao_bytes EQUAL 0)
            message(FATAL_ERROR "ambient occlusion off still costs passes or memory:\n${graph}")
        endif()
    elseif(NOT graph MATCHES "surface prepass" OR NOT graph MATCHES "ambient occlusion" OR surface_bytes EQUAL 0
           OR ao_bytes EQUAL 0)
        message(FATAL_ERROR "ambient occlusion ${ao} is missing its passes or memory:\n${graph}")
    endif()
endforeach()
file(REMOVE_RECURSE "${TEMP}")
