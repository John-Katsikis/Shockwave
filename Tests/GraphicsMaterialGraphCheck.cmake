# Material graphs (W4, docs/MaterialGraph.md) on the GPU, with the graphics fixtures
# (Tools/material_graph.py):
#   - a graph that works out a standard material's own values in nodes draws the same picture;
#   - material_graphs false draws each graph material exactly as its plain material;
#   - the lava graph's glow changes the picture, graph_param glow 0 takes it away, and two
#     fixed-step runs of its scrolling noise are identical;
#   - the report counts the graph draws and shaders, and none of either when off.
cmake_policy(SET CMP0054 NEW)   # quoted strings in if() are never variables
file(MAKE_DIRECTORY "${TEMP}")
function(render scene name)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/${scene}.swscene" --fixed-step --size 640x360 --frames 5 ${ARGN}
        --screenshot "${TEMP}/${name}.png" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "[exit ${result}] ${scene} (${ARGN}) didn't render: ${out}\n${err}")
    endif()
endfunction()
function(rmse a b region out)
    execute_process(COMMAND "${PLAYER}" --image-diff "${TEMP}/${a}.png" "${TEMP}/${b}.png" --tolerance 255
        --region "${region}" --json "${TEMP}/diff.json" OUTPUT_QUIET)
    file(READ "${TEMP}/diff.json" diff)
    string(JSON value GET "${diff}" rmse)
    set(${out} "${value}" PARENT_SCOPE)
endfunction()
function(luma image region out)
    execute_process(COMMAND "${PLAYER}" --image-stats "${TEMP}/${image}.png" --region "${region}"
        --json "${TEMP}/stats.json" OUTPUT_QUIET)
    file(READ "${TEMP}/stats.json" stats)
    string(JSON value GET "${stats}" mean_luma)
    string(REGEX REPLACE "\\..*" "" value "${value}")   # whole levels: math() has no floats
    set(${out} "${value}" PARENT_SCOPE)
endfunction()
function(report scene name)
    execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --no-user-graphics
        --scene "${FIXTURE}/Scenes/${scene}.swscene" --size 320x180 ${ARGN} --graphics-report --json "${TEMP}/${name}.json"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "[exit ${result}] --graphics-report (${scene} ${ARGN}) failed: ${out}\n${err}")
    endif()
    file(READ "${TEMP}/${name}.json" json)
    set(json "${json}" PARENT_SCOPE)
endfunction()

set(box "0.52,0.38,0.24,0.4")   # the box, where the lava glows most

# A forwarding graph is its standard material.
render(graph-plain plain)
render(graph-forward forward)
rmse(forward plain "0,0,1,1" forward_change)
message(STATUS "material graph: the forwarding graph differs ${forward_change} from its standard material")
if(NOT forward_change LESS 0.5)
    message(FATAL_ERROR "a graph working out a standard material's values doesn't draw the same (RMSE ${forward_change})")
endif()

# Off: exactly the plain material.
render(graph-lava lava)
render(graph-lava lava-again)
render(graph-lava lava-off --graphics-set material_graphs=false)
render(graph-lava-plain lava-plain)
render(graph-lava-cold lava-cold)
rmse(lava-off lava-plain "0,0,1,1" off_change)
if(NOT off_change EQUAL 0)
    message(FATAL_ERROR "material_graphs false isn't exactly the plain material: RMSE ${off_change}")
endif()
rmse(lava-again lava "0,0,1,1" repeat)
if(NOT repeat EQUAL 0)
    message(FATAL_ERROR "two fixed-step runs of the lava graph differ: RMSE ${repeat}")
endif()
rmse(lava lava-plain "${box}" glow_change)
luma(lava "${box}" hot)
luma(lava-cold "${box}" cold)
message(STATUS "material graph: lava changes the box by ${glow_change}; its luma ${hot}, with glow 0 ${cold}")
if(glow_change LESS 5)
    message(FATAL_ERROR "the lava graph doesn't change the box (RMSE ${glow_change})")
endif()
math(EXPR margin "${cold} + 10")
if(NOT hot GREATER margin)
    message(FATAL_ERROR "graph_param glow 0 doesn't take the glow away (luma ${hot} with it, ${cold} without)")
endif()

# The report.
report(graph-lava lava-report)
string(JSON draws GET "${json}" material_graphs draws)
string(JSON shaders GET "${json}" material_graphs shaders)
string(JSON errors GET "${json}" material_graphs errors)
message(STATUS "material graph: ${draws} draws with ${shaders} graph shaders loaded")
if(draws LESS 2 OR shaders LESS 1 OR NOT errors EQUAL 0)
    message(FATAL_ERROR "the report doesn't count the lava's graph draws: ${json}")
endif()
report(graph-lava lava-off-report --graphics-set material_graphs=false)
string(JSON draws GET "${json}" material_graphs draws)
string(JSON enabled GET "${json}" material_graphs enabled)
if(NOT draws EQUAL 0 OR enabled)
    message(FATAL_ERROR "material_graphs false still draws graphs (${draws} draws)")
endif()
