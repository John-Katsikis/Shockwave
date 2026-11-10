# S10 part 2 on the GPU: pipeline warm-up and the fallback tier's pictures.
#   -DPLAYER=player -DFIXTURE=Tests/fixtures/graphics -DTEMP=dir [-DDEMO=Games/ShippingDemo]
#
# Warm-up (docs/ReleaseTier.md): glass and skin pipelines are built when their materials
# load, so none is built by a draw; with --no-pipeline-warmup they're built by the first
# draw, as before; an effect that's off gets no pipelines either way; and the picture is
# the same to the pixel whichever way they were built.
#
# The fallback tier (with DEMO): Reactor Run's factory drawn with --graphics-preset
# fallback isn't empty, is the same picture twice, has the shadows its tier calls
# mandatory, and isn't the high preset's picture.
cmake_policy(SET CMP0054 NEW)
file(MAKE_DIRECTORY "${TEMP}")
function(pipelines scene name)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/${scene}.swscene" --size 320x180 --no-pipeline-cache ${ARGN}
        --graphics-report --json "${TEMP}/${name}.json"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "[exit ${result}] --graphics-report (${scene} ${ARGN}) failed: ${out}\n${err}")
    endif()
    file(READ "${TEMP}/${name}.json" json)
    foreach(field created warmed warmup while_drawing)
        string(JSON value GET "${json}" pipelines ${field})
        set(${field} "${value}" PARENT_SCOPE)
    endforeach()
endfunction()

# The fixture project loads every material at startup, so glass and skin are both asked for.
pipelines(shading-glass warm)
set(warm_created ${created})
if(NOT warmup STREQUAL "ON" AND NOT warmup STREQUAL "true")
    message(FATAL_ERROR "warm-up should be on by default (${warmup})")
endif()
if(warmed EQUAL 0 OR NOT while_drawing EQUAL 0)
    message(FATAL_ERROR "warm-up on: ${warmed} pipelines warmed, ${while_drawing} built while drawing (expected some, and 0)")
endif()
pipelines(shading-glass cold --no-pipeline-warmup)
if(NOT warmed EQUAL 0 OR while_drawing EQUAL 0)
    message(FATAL_ERROR "warm-up off: ${warmed} pipelines warmed, ${while_drawing} built while drawing (expected 0, and some)")
endif()
if(NOT created LESS warm_created)
    message(FATAL_ERROR "warm-up off built ${created} pipelines, no fewer than the ${warm_created} with it on: the glass scene never draws skin")
endif()
message(STATUS "warm-up: ${warm_created} pipelines with it, ${created} without (${while_drawing} of them while drawing)")
# Off means no pipelines: the same scene with glass and skin off builds none of theirs.
pipelines(shading-glass off --graphics-set transmission_quality=off --graphics-set skin_quality=off)
if(NOT warmed EQUAL 0 OR NOT while_drawing EQUAL 0)
    message(FATAL_ERROR "glass and skin off still built pipelines: ${warmed} warmed, ${while_drawing} while drawing")
endif()
# A scene without such materials, in a project without them, would warm nothing; here the
# project has them, so a plain scene warms them too and still builds none while drawing.
pipelines(thin-geometry plain)
if(NOT while_drawing EQUAL 0)
    message(FATAL_ERROR "a plain scene built ${while_drawing} pipelines while drawing")
endif()

# The same pixels however the pipelines were built.
foreach(mode warm cold)
    set(flag "")
    if(mode STREQUAL "cold")
        set(flag --no-pipeline-warmup)
    endif()
    foreach(scene shading-glass shading-skin)
        execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
            --scene "${FIXTURE}/Scenes/${scene}.swscene" --fixed-step --size 320x180 --frames 5 ${flag}
            --screenshot "${TEMP}/${scene}-${mode}.png" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "[exit ${result}] ${scene} (${mode}) didn't render: ${out}\n${err}")
        endif()
    endforeach()
endforeach()
foreach(scene shading-glass shading-skin)
    execute_process(COMMAND "${PLAYER}" --image-diff "${TEMP}/${scene}-warm.png" "${TEMP}/${scene}-cold.png"
        --tolerance 0 RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "[exit ${result}] ${scene} differs with warm-up on and off: ${out}\n${err}")
    endif()
endforeach()

if(NOT DEFINED DEMO)
    return()
endif()

# ---- The fallback tier's pictures ----
function(factory name)
    execute_process(COMMAND "${PLAYER}" --project "${DEMO}" --no-user-graphics --user-dir "${TEMP}/user"
        --scene "${DEMO}/Scenes/factory.swscene" --fixed-step --size 640x360 --frames 10 ${ARGN}
        --screenshot "${TEMP}/${name}.png" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "[exit ${result}] the factory (${ARGN}) didn't render: ${out}\n${err}")
    endif()
endfunction()
function(differ a b out_var)
    execute_process(COMMAND "${PLAYER}" --image-diff "${TEMP}/${a}.png" "${TEMP}/${b}.png" --tolerance 0
        RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()
factory(fallback-1 --graphics-preset fallback)
factory(fallback-2 --graphics-preset fallback)
factory(fallback-as-low --graphics-preset low)
factory(fallback-no-shadows --graphics-preset fallback --graphics-set shadow_quality=off)
factory(high --graphics-preset high)
execute_process(COMMAND "${PLAYER}" --image-stats "${TEMP}/fallback-1.png" RESULT_VARIABLE result OUTPUT_VARIABLE stats)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the fallback tier's picture is empty: ${stats}")
endif()
differ(fallback-1 fallback-2 same)
if(NOT same EQUAL 0)
    message(FATAL_ERROR "the fallback tier draws a different picture each run")
endif()
differ(fallback-1 fallback-as-low same)
if(NOT same EQUAL 0)
    message(FATAL_ERROR "Reactor Run's fallback is low, but --graphics-preset fallback isn't the low preset's picture")
endif()
differ(fallback-1 fallback-no-shadows shadows)
if(shadows EQUAL 0)
    message(FATAL_ERROR "the fallback tier's picture has no shadows: the tier calls them mandatory")
endif()
differ(fallback-1 high tiers)
if(tiers EQUAL 0)
    message(FATAL_ERROR "the fallback tier draws exactly what high draws")
endif()
