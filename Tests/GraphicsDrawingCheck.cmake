# Batching, occlusion culling and indirect drawing (G12, docs/Drawing.md) on the GPU, with
# the graphics fixtures:
#   - instancing and occlusion culling, each on and off, render exactly the same pictures:
#     the crowded city (sky gaps, masked foliage, glass, a shadow from off camera), with
#     ambient occlusion and with TAA, at an odd size too; the dense occluder; glass panels;
#   - the report says what happened: fewer calls than objects, draws occluded, the depth
#     pyramid's memory, and why culling didn't run when it didn't;
#   - each setting off costs nothing: one call an object, no pyramid, no culling pass;
#   - the pipeline cache is off when asked, and kept otherwise.
cmake_policy(SET CMP0054 NEW)   # quoted strings in if() are never variables
file(MAKE_DIRECTORY "${TEMP}")
function(render scene name size)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/${scene}.swscene" --fixed-step --size ${size} --frames 6 ${ARGN}
        --screenshot "${TEMP}/${name}.png" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${scene} (${ARGN}) didn't render: ${out}\n${err}")
    endif()
endfunction()
function(report scene name)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/${scene}.swscene" --size 640x360 ${ARGN} --graphics-report
        --json "${TEMP}/${name}.json" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "--graphics-report (${scene} ${ARGN}) failed: ${out}\n${err}")
    endif()
    file(READ "${TEMP}/${name}.json" json)
    string(JSON drawing GET "${json}" drawing)
    string(JSON graph GET "${json}" frame_graph)
    set(drawing "${drawing}" PARENT_SCOPE)
    set(graph "${graph}" PARENT_SCOPE)
endfunction()
function(identical a b what)
    execute_process(COMMAND "${PLAYER}" --image-diff "${TEMP}/${a}.png" "${TEMP}/${b}.png" --tolerance 255
        --json "${TEMP}/diff.json" OUTPUT_QUIET)
    file(READ "${TEMP}/diff.json" diff)
    string(JSON value GET "${diff}" rmse)
    if(NOT value EQUAL 0)
        message(FATAL_ERROR "${what}: RMSE ${value} between ${a}.png and ${b}.png")
    endif()
endfunction()
function(field key out)
    string(JSON value GET "${drawing}" ${key})
    set(${out} "${value}" PARENT_SCOPE)
endfunction()

set(off --graphics-set instancing=false --graphics-set occlusion_culling=off)
set(on --graphics-set instancing=true --graphics-set occlusion_culling=on)

# ---- Pictures: every combination matches everything off ----
foreach(setup "ao;640x360;--graphics-set;ambient_occlusion=medium;--graphics-set;anti_aliasing=none"
              "taa;640x360;--graphics-set;ambient_occlusion=off;--graphics-set;anti_aliasing=taa"
              "odd;643x359;--graphics-set;ambient_occlusion=medium;--graphics-set;anti_aliasing=fxaa")
    list(POP_FRONT setup tag size)
    render(crowded-city city-${tag}-off ${size} ${setup} ${off})
    foreach(instancing true false)
        foreach(culling on off)
            render(crowded-city city-${tag}-${instancing}-${culling} ${size} ${setup}
                --graphics-set instancing=${instancing} --graphics-set occlusion_culling=${culling})
            identical(city-${tag}-off city-${tag}-${instancing}-${culling}
                "crowded city (${tag}), instancing ${instancing}, occlusion culling ${culling}")
        endforeach()
    endforeach()
endforeach()
render(crowded-city city-overhead-off 640x360 --camera Overhead --graphics-set ambient_occlusion=medium ${off})
render(crowded-city city-overhead-on 640x360 --camera Overhead --graphics-set ambient_occlusion=medium ${on})
identical(city-overhead-off city-overhead-on "crowded city from overhead (sees every prop)")
foreach(scene dense-occluder transparency)
    render(${scene} ${scene}-off 640x360 --graphics-set ambient_occlusion=medium ${off})
    render(${scene} ${scene}-on 640x360 --graphics-set ambient_occlusion=medium ${on})
    identical(${scene}-off ${scene}-on "${scene}, instancing and occlusion culling on")
endforeach()

# ---- The report ----
report(crowded-city city-on --graphics-set ambient_occlusion=medium ${on})
field(objects objects)
field(calls calls)
field(instanced instanced)
field(occlusion_active active)
field(tested tested)
field(occluded occluded)
field(hzb_bytes hzb)
if(NOT active STREQUAL "ON" AND NOT active STREQUAL "true")
    field(occlusion_reason reason)
    message(FATAL_ERROR "occlusion culling didn't run on the crowded city: ${reason}")
endif()
math(EXPR halfObjects "${objects} / 2")
if(NOT calls LESS halfObjects OR instanced EQUAL 0)
    message(FATAL_ERROR "instancing: ${calls} calls for ${objects} objects (${instanced} instanced)")
endif()
math(EXPR halfTested "${tested} / 2")
if(tested EQUAL 0 OR NOT occluded GREATER halfTested)
    message(FATAL_ERROR "occlusion culling hid ${occluded} of ${tested}: most of the city's yards are hidden")
endif()
if(hzb EQUAL 0 OR NOT graph MATCHES "occlusion culling")
    message(FATAL_ERROR "occlusion culling ran without a depth pyramid or pass: ${hzb} bytes; ${graph}")
endif()
message(STATUS "crowded city: ${objects} objects in ${calls} calls, ${occluded} of ${tested} occluded")

report(crowded-city city-off --graphics-set ambient_occlusion=medium ${off})
field(objects objects)
field(calls calls)
field(hzb_bytes hzb)
field(occlusion_reason reason)
if(NOT calls EQUAL objects OR NOT hzb EQUAL 0 OR graph MATCHES "occlusion culling")
    message(FATAL_ERROR "with both off: ${calls} calls for ${objects} objects, ${hzb} pyramid bytes; ${graph}")
endif()
if(NOT reason STREQUAL "occlusion_culling off")
    message(FATAL_ERROR "occlusion culling off reports \"${reason}\"")
endif()

# No depth prepass this frame: culling stays off and says why, and adds no pass.
report(crowded-city city-noprepass --graphics-set ambient_occlusion=off --graphics-set anti_aliasing=fxaa
    --graphics-set decals_quality=off ${on})
field(occlusion_reason reason)
field(hzb_bytes hzb)
if(NOT reason MATCHES "no depth prepass" OR NOT hzb EQUAL 0 OR graph MATCHES "occlusion culling|surface prepass")
    message(FATAL_ERROR "without a prepass: \"${reason}\", ${hzb} pyramid bytes; ${graph}")
endif()

# ---- The pipeline cache ----
report(dense-occluder cache-off --no-pipeline-cache)
string(JSON state GET "${drawing}" pipeline_cache state)
if(NOT state STREQUAL "off")
    message(FATAL_ERROR "--no-pipeline-cache still has a pipeline cache: ${state}")
endif()
report(dense-occluder cache-on)
string(JSON state GET "${drawing}" pipeline_cache state)
string(JSON file GET "${drawing}" pipeline_cache file)
if(state STREQUAL "off" OR NOT file MATCHES "pipeline-cache/.+\\.swpipelines$")
    message(FATAL_ERROR "the pipeline cache isn't kept: ${state}, ${file}")
endif()
