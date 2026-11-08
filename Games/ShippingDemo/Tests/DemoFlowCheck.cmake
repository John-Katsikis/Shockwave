# Reactor Run through the player (S07): --check on the project, then the whole game with
# real input through the device boundary (keyboard and mouse, then a gamepad), each from
# a fresh profile; then save and quit in the factory and Continue in a second run. Then
# the Settings page and the profile it saves. Then the same from a package
# (Config/Package.swpackage), run from outside the source tree,
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

# The Settings page (S08): a volume, the language and a display change, applied and kept;
# then the saved profile, as the next run and --preferences read it.
flow(settings settings)
# S08 part 2: the gamepad in use unplugged mid-level pauses and says so; the keyboard
# carries on, and the pad works again once it's back.
flow(controller controller)
file(GLOB_RECURSE profiles "${TEMP}/settings/*Preferences.swprefs")
list(LENGTH profiles count)
if(NOT count EQUAL 1)
    message(FATAL_ERROR "the Settings flow saved ${count} profiles: ${profiles}")
endif()
file(READ "${profiles}" profile)
foreach(saved "music_volume 0.8" "language fr" "window_mode borderless")
    if(NOT profile MATCHES "${saved}")
        message(FATAL_ERROR "the saved preferences don't have ${saved}:\n${profile}")
    endif()
endforeach()
execute_process(COMMAND "${PLAYER}" --project "${PROJECT}" --user-dir "${TEMP}/settings" --preferences
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0 OR NOT out MATCHES "language +fr +# profile" OR NOT out MATCHES "ui_scale +1 +# default")
    message(FATAL_ERROR "player --preferences (${code}) doesn't show the saved profile:\n${out}\n${err}")
endif()
execute_process(COMMAND "${PLAYER}" --project "${PROJECT}" --user-dir "${TEMP}/settings" --pref language=en
        --pref ui_scale=1.5 --preferences --json -
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0 OR NOT out MATCHES "\"key\": *\"ui_scale\", *\"group\": *\"Accessibility\", *\"value\": *\"1.5\", *\"origin\": *\"session\"")
    message(FATAL_ERROR "player --pref ... --preferences --json (${code}):\n${out}\n${err}")
endif()
execute_process(COMMAND "${PLAYER}" --project "${PROJECT}" --user-dir "${TEMP}/settings" --pref ui_scale=9 --preferences
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(code EQUAL 0 OR NOT "${out}${err}" MATCHES "ui_scale")
    message(FATAL_ERROR "player --pref ui_scale=9 should fail, saying why (${code}):\n${out}\n${err}")
endif()
# The rebound control: saved by action id beside the preferences, and only that one.
file(GLOB_RECURSE controls "${TEMP}/settings/*Controls.swinput")
if(NOT controls)
    message(FATAL_ERROR "the Settings flow saved no Controls.swinput")
endif()
file(READ "${controls}" saved)
if(NOT saved MATCHES "bind demo_fire key F" OR NOT saved MATCHES "bind demo_fire pad_axis right_trigger" OR
   saved MATCHES "demo_reload")
    message(FATAL_ERROR "the saved controls aren't the one rebound:\n${saved}")
endif()
message(STATUS "preferences from the Settings page: ok")

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
# The packaged Settings page: the catalogs went into the package (the French reply).
package_flow(settings settings)
