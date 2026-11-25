# The Material graph window (W4b, docs/MaterialGraph.md#the-editor): on a copy of the graphics
# fixtures, `editor --material-graph-test` opens forward.swmatgraph with the window's own
# code, adds a constant wired to roughness, waits for the background compile, checks that a
# broken node's problem lands on that node while the working shader stays, undoes it and
# saves, and that the preview sphere draws with the graph. Then the saved file is checked
# here, and by the player.
#   -DEDITOR=editor -DPLAYER=player -DFIXTURE=Tests/fixtures/graphics -DTEMP=dir
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}")
file(COPY "${FIXTURE}/" DESTINATION "${TEMP}/graphics")
set(project "${TEMP}/graphics")
set(graph "${project}/Content/materials/graph/forward.swmatgraph")
execute_process(COMMAND "${EDITOR}" "${project}/Graphics.swproject" --prefs "${TEMP}/prefs"
                        --material-graph materials/graph/forward.swmatgraph --material-graph-test
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0 OR NOT out MATCHES "material graph editor: added ([a-z0-9_]+), .*; ok")
    message(FATAL_ERROR "editor --material-graph-test: exit ${result}\n${out}\n${err}")
endif()
set(added "${CMAKE_MATCH_1}")
file(READ "${graph}" text)
foreach(want "node ${added} constant 0.123" "output roughness ${added}" "# layout ${added} ")
    string(FIND "${text}" "${want}" at)
    if(at LESS 0)
        message(FATAL_ERROR "the saved graph lacks \"${want}\":\n${text}")
    endif()
endforeach()
execute_process(COMMAND "${PLAYER}" --material-graph "${graph}"
                RESULT_VARIABLE result OUTPUT_VARIABLE info ERROR_VARIABLE err)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "player --material-graph on the saved graph: exit ${result}\n${info}\n${err}")
endif()
message(STATUS "material graph editor: added ${added}, compiled, saved with its layout line")
