# Reactor Run through the player (S07): --check on the project, then the whole game with
# real input through the device boundary (keyboard and mouse, then a gamepad), each from
# a fresh profile; then save and quit in the factory and Continue in a second run. Then
# the same from a package (Config/Package.swpackage), run from outside the source tree,
# the way a tester gets it: the whole game, and save, quit and Continue.
# cmake -DPLAYER=... -DPROJECT=... -DFLOWS=... -DTEMP=... [-DPACKAGE=ON] -P DemoFlowCheck.cmake
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

# The packaged game (S07 completion): packaged through the real CLI without rebuilding,
# then played from the package's own launcher with a fresh profile.
# Packaging needs a modular build off Windows (docs/Packaging.md), like package_cli.
if(NOT PACKAGE)
    return()
endif()
execute_process(COMMAND "${PLAYER}" --package --project "${PROJECT}" --no-build --no-archive
        --output "${TEMP}/package"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0 OR NOT out MATCHES "\nrun ([^\r\n]+)")
    message(FATAL_ERROR "player --package failed (${code}):\n${out}\n${err}")
endif()
set(launcher "${CMAKE_MATCH_1}")
if(NOT launcher MATCHES "ReactorRun")
    message(FATAL_ERROR "the package's launcher is ${launcher}, not ReactorRun (Config/Package.swpackage)")
endif()
get_filename_component(package "${launcher}" DIRECTORY)
foreach(left_out Game/Docs Game/Tests Game/Source)
    if(EXISTS "${package}/${left_out}")
        message(FATAL_ERROR "the package has ${left_out}, which Config/Package.swpackage leaves out")
    endif()
endforeach()
function(package_flow name user)
    execute_process(COMMAND "${launcher}" --renderer null --user-dir "${TEMP}/package-${user}"
            --flow-test "${FLOWS}/${name}.json" --json "${TEMP}/package-${name}-report.json"
        WORKING_DIRECTORY "${TEMP}"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "${name} from the package failed (${code}):\n${out}\n${err}")
    endif()
    message(STATUS "${name} from the package: ok")
endfunction()
package_flow(full-run keyboard)
package_flow(resume-quit resume)
package_flow(resume-continue resume)
