# Reactor Run through the player (S07): --check on the project, then the whole game with
# real input through the device boundary (keyboard and mouse, then a gamepad), each from
# a fresh profile; then save and quit in the factory and Continue in a second run.
# cmake -DPLAYER=... -DPROJECT=... -DFLOWS=... -DTEMP=... -P DemoFlowCheck.cmake
foreach(var PLAYER PROJECT FLOWS TEMP)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "DemoFlowCheck.cmake needs -D${var}=...")
    endif()
endforeach()
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}")

function(player_run name)
    execute_process(COMMAND "${PLAYER}" --project "${PROJECT}" ${ARGN}
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "${name} failed (${code}):\n${out}\n${err}")
    endif()
    message(STATUS "${name}: ok")
endfunction()

player_run("--check" --check)

function(flow name user)
    player_run("${name}" --renderer null --user-dir "${TEMP}/${user}"
        --flow-test "${FLOWS}/${name}.json" --json "${TEMP}/${name}-report.json")
endfunction()

flow(full-run keyboard)
flow(gamepad-run gamepad)
flow(resume-quit resume)
flow(resume-continue resume)
