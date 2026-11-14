# Recovering from failures (S12, docs/Diagnostics.md), headless on a copy of
# Tests/fixtures/levels with the null renderer:
#   - an injected lost GPU device stops the game (exit 1) without drawing again, says the
#     saves are as they were, and leaves the session marker saying so;
#   - --diagnostics sees that run; the next game run warns about it;
#   - the menu then opens on the recovery page: Safe graphics for the session (the player's
#     graphics file isn't written), Restore last save loads the save made before;
#   - --no-recovery-prompt turns the page off; a clean run ends it;
#   - after a real crash (crash_program, POSIX) the page names its report and goes aside
#     for the main menu;
#   - --inject-device-lost is a usage error in a packaged game's place (frame 0).
#
#   cmake -DPLAYER=player [-DCRASH=crash_program] -DFIXTURE=Tests/fixtures/levels
#         -DSCENARIOS=Tests/fixtures/flow -DTEMP=dir -P RecoveryCheck.cmake
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}")
file(COPY "${FIXTURE}/" DESTINATION "${TEMP}/Levels")
set(project "${TEMP}/Levels/Levels.swproject")
set(user "${TEMP}/user")

function(run expected pattern)
    execute_process(COMMAND "${PLAYER}" --project "${project}" --renderer null --app-shell all --user-dir "${user}"
                            ${ARGN}
                    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL ${expected} OR NOT "${out}${err}" MATCHES "${pattern}")
        message(FATAL_ERROR "player ${ARGN}: expected exit ${expected} and '${pattern}', got ${result}:\n${out}\n${err}")
    endif()
    set(last "${out}${err}" PARENT_SCOPE)
endfunction()
function(lacks pattern)
    if("${last}" MATCHES "${pattern}")
        message(FATAL_ERROR "didn't expect '${pattern}' in:\n${last}")
    endif()
endfunction()

# A save to come back to.
run(0 "flow test passed: 6 steps" --flow-test "${SCENARIOS}/recovery-save.json")

# The GPU device is lost at frame 3: nothing more is drawn, the game stops.
run(1 "GPU device was lost \\(injected for testing.*your saves are as they were" --frames 300 --inject-device-lost 3)
lacks("Restarting")   # no display here: no dialog, no restart
file(READ "${user}/Logs/player.session" marker)
if(NOT marker MATCHES "renderer_state device lost")
    message(FATAL_ERROR "the session marker should say the device was lost:\n${marker}")
endif()
run(0 "previous run unclean" --diagnostics)

# The menu's recovery page: safe graphics for the session, then the last save.
run(0 "flow test passed: 10 steps" --flow-test "${SCENARIOS}/recovery.json")
if(EXISTS "${user}/Config/Graphics.swgraphics")
    message(FATAL_ERROR "Safe graphics from the menu wrote the player's graphics file")
endif()
# Switched off.
run(0 "flow test passed: 3 steps" --no-recovery-prompt --flow-test "${SCENARIOS}/recovery-off.json")

# A game run warns (no dialog without a display), then ends cleanly: no page after it.
run(0 "didn't end cleanly \\(its GPU device was lost\\).*After the last run: no dialog shown" --frames 5)
run(0 "previous run clean" --diagnostics)
run(0 "flow test passed: 3 steps" --flow-test "${SCENARIOS}/recovery-off.json")

# A real crash: the page names its report, and goes aside for the main menu.
if(CRASH)
    execute_process(COMMAND "${CRASH}" "${user}/Crashes" segfault "${user}/Logs" player
                    RESULT_VARIABLE code OUTPUT_QUIET ERROR_QUIET)
    run(0 "flow test passed: 5 steps" --flow-test "${SCENARIOS}/recovery-dismiss.json")
    run(0 "previous run's report" --diagnostics)
endif()

# Fault injection takes a real frame number.
run(2 "--inject-device-lost takes" --inject-device-lost 0)
