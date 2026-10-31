# Clustered local lighting (G11) on the GPU, with the graphics fixtures:
#   - clustered and brute force render the same pixels (RMSE 0, no pixel off by even 1) on
#     many point and spot lights with shadows and glass, past cluster_far, from above, with
#     TAA's jitter, and where 96 lights overflow the lists;
#   - the report: the "light clusters" pass, its memory and its counts when clustered (and
#     the overflow), nothing at all with brute_force, and auto choosing by light count;
#   - the cluster view shows the overflow in magenta.
# With TIMING set it instead requires the scene pass to be faster clustered on 200 lights.
cmake_policy(SET CMP0054 NEW)   # quoted strings in if() are never variables
file(MAKE_DIRECTORY "${TEMP}")
function(render scene name)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/${scene}.swscene" --fixed-step --size 320x180 --frames 5 ${ARGN}
        --screenshot "${TEMP}/${name}.png" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${scene} (${ARGN}) didn't render: ${out}\n${err}")
    endif()
endfunction()
function(report scene name)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/${scene}.swscene" --size 320x180 ${ARGN} --graphics-report --json "${TEMP}/${name}.json"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "--graphics-report (${scene} ${ARGN}) failed: ${out}\n${err}")
    endif()
    file(READ "${TEMP}/${name}.json" json)
    set(json "${json}" PARENT_SCOPE)
endfunction()

if(TIMING)
    # Scene pass GPU time on 200 point and 4 spot lights, brute force against clustered.
    foreach(mode brute_force clustered)
        execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
            --scene "${FIXTURE}/Scenes/many-lights.swscene" --size 640x360 --graphics-set light_culling=${mode}
            --benchmark 2 --json "${TEMP}/bench-${mode}.json" RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
        file(READ "${TEMP}/bench-${mode}.json" json)
        string(JSON count LENGTH "${json}" passes)
        math(EXPR last "${count} - 1")
        set(ms_${mode} 0)
        foreach(i RANGE ${last})
            string(JSON pass GET "${json}" passes ${i} name)
            if(pass STREQUAL "scene" OR pass STREQUAL "light clusters")
                string(JSON ms GET "${json}" passes ${i} gpu_ms)
                string(REGEX REPLACE "\\..*" "" ms "${ms}")   # whole milliseconds: math() has no floats
                math(EXPR ms_${mode} "${ms_${mode}} + ${ms}")
            endif()
        endforeach()
    endforeach()
    message(STATUS "lighting GPU ms: brute force ${ms_brute_force}, clustered ${ms_clustered} (scene + clusters)")
    if(NOT ms_clustered LESS ms_brute_force)
        message(FATAL_ERROR "clustered lighting isn't faster on many-lights: ${ms_clustered} vs ${ms_brute_force} ms")
    endif()
    return()
endif()

# ---- The same pixels, clustered or not ----
foreach(case "many-lights;main" "many-lights;overhead;--camera;Overhead" "many-lights;taa;--anti-aliasing;taa"
        "light-overflow;main" "lights-overlap;main" "transparency;main")
    list(GET case 0 scene)
    list(GET case 1 name)
    set(extra "")
    list(LENGTH case length)
    if(length GREATER 2)
        list(SUBLIST case 2 -1 extra)
    endif()
    render(${scene} ${scene}-${name}-brute ${extra} --graphics-set light_culling=brute_force)
    render(${scene} ${scene}-${name}-clustered ${extra} --graphics-set light_culling=clustered)
    execute_process(COMMAND "${PLAYER}" --image-diff "${TEMP}/${scene}-${name}-brute.png"
        "${TEMP}/${scene}-${name}-clustered.png" --tolerance 0 --json "${TEMP}/diff.json" OUTPUT_QUIET)
    file(READ "${TEMP}/diff.json" diff)
    string(JSON max GET "${diff}" max_diff)
    if(NOT max EQUAL 0)
        message(FATAL_ERROR "${scene} (${name}): clustered lighting differs from brute force:\n${diff}")
    endif()
endforeach()

# ---- Reports ----
report(many-lights clustered --graphics-set light_culling=clustered)
string(JSON graph GET "${json}" frame_graph)
string(JSON bytes GET "${json}" memory categories "light clusters" bytes)
string(JSON clustered GET "${json}" clusters clustered)
string(JSON locals GET "${json}" clusters local_lights)
string(JSON most GET "${json}" clusters max_candidates)
string(JSON overflow GET "${json}" clusters overflow_clusters)
if(NOT graph MATCHES "light clusters" OR bytes EQUAL 0 OR NOT clustered OR NOT locals EQUAL 204 OR most EQUAL 0
   OR NOT overflow EQUAL 0)
    message(FATAL_ERROR "many-lights clustered: pass, memory or counts wrong (${bytes} bytes): ${json}")
endif()
report(light-overflow overflow --graphics-set light_culling=clustered)
string(JSON most GET "${json}" clusters max_candidates)
string(JSON overflow GET "${json}" clusters overflow_clusters)
if(NOT most GREATER 64 OR overflow EQUAL 0)
    message(FATAL_ERROR "light-overflow: no cluster reported overflowing (most ${most}, ${overflow} overflowed)")
endif()
report(many-lights brute --graphics-set light_culling=brute_force)
string(JSON graph GET "${json}" frame_graph)
string(JSON bytes GET "${json}" memory categories "light clusters" bytes)
string(JSON clustered GET "${json}" clusters clustered)
if(graph MATCHES "light clusters" OR NOT bytes EQUAL 0 OR clustered)
    message(FATAL_ERROR "light_culling brute_force still costs a cluster pass or memory (${bytes} bytes):\n${graph}")
endif()
report(many-lights auto-many)
string(JSON clustered GET "${json}" clusters clustered)
if(NOT clustered)
    message(FATAL_ERROR "auto didn't cluster 204 point and spot lights: ${json}")
endif()
report(lights-overlap auto-few)
string(JSON clustered GET "${json}" clusters clustered)
string(JSON graph GET "${json}" frame_graph)
if(clustered OR graph MATCHES "light clusters")
    message(FATAL_ERROR "auto clustered 4 point and spot lights")
endif()

# ---- The cluster view: overflowing clusters are magenta ----
render(light-overflow overflow-view --graphics-set light_culling=clustered --view clusters)
execute_process(COMMAND "${PLAYER}" --image-stats "${TEMP}/overflow-view.png" --region "0.3,0.6,0.4,0.3"
    --json "${TEMP}/stats.json" OUTPUT_QUIET)
file(READ "${TEMP}/stats.json" stats)
string(JSON r GET "${stats}" mean_rgb 0)
string(JSON g GET "${stats}" mean_rgb 1)
string(JSON b GET "${stats}" mean_rgb 2)
if(r LESS 200 OR g GREATER 40 OR b LESS 200)
    message(FATAL_ERROR "the overflowing floor isn't magenta in the cluster view: ${r} ${g} ${b}")
endif()
