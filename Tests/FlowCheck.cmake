# --flow-test on a copy of Tests/fixtures/levels (S05, docs/FlowTest.md), headless:
#   - the milestone: the menu's New game, a loading screen, A, B, Restart level from the
#     pause screen, Main menu, all through the shell's buttons (Tests/fixtures/flow);
#   - refusals: an unknown level, a missing spawn, Continue without a save;
#   - a save and, from the pause screen, a load;
#   - a level that doesn't load, and a menu level that doesn't: the error screen, Retry,
#     Main menu and back to the level;
#   - a failing step exits 1 with the step, the report says where things stood;
#   - a bad scenario is a usage error (exit 2); without the shell a click can't happen;
#   - the project's app_shell key turns the screens on.
# With -DRENDERER=vulkan only the milestone runs, drawing every screen on the GPU.
#   -DPLAYER=player -DFIXTURE=Tests/fixtures/levels -DSCENARIOS=Tests/fixtures/flow -DTEMP=dir
cmake_policy(SET CMP0054 NEW)
if(NOT RENDERER)
    set(RENDERER null)
endif()
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}")
file(COPY "${FIXTURE}/" DESTINATION "${TEMP}/Levels")
set(root "${TEMP}/Levels")
set(project "${root}/Levels.swproject")
file(READ "${root}/Scenes/a.swscene" goodA)
file(READ "${root}/Scenes/menu.swscene" goodMenu)

function(run expected pattern)
    execute_process(COMMAND "${PLAYER}" --project "${project}" --renderer ${RENDERER} ${ARGN}
                    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL ${expected} OR NOT "${out}${err}" MATCHES "${pattern}")
        message(FATAL_ERROR "player ${ARGN}: expected exit ${expected} and '${pattern}', got ${result}:\n${out}\n${err}")
    endif()
endfunction()

function(expect file pattern)
    file(READ "${file}" text)
    if(NOT text MATCHES "${pattern}")
        message(FATAL_ERROR "${file}: missing '${pattern}' in:\n${text}")
    endif()
endfunction()

# The milestone, with its report.
run(0 "flow test passed: 23 steps" --app-shell all --flow-test "${SCENARIOS}/milestone.json"
    --json "${TEMP}/milestone.json")
expect("${TEMP}/milestone.json" "\"ok\": true")
expect("${TEMP}/milestone.json" "\"level\": \"menu\", \"epoch\": 5")
expect("${TEMP}/milestone.json" "\"level\": \"b\", \"from\": \"b\", \"to\": \"b\", \"status\": \"succeeded\"")
if(NOT RENDERER STREQUAL "null")
    return()
endif()
# The same, from the project's own setting.
file(READ "${project}" goodProject)
file(WRITE "${project}" "${goodProject}app_shell all\n")
run(0 "flow test passed: 23 steps" --flow-test "${SCENARIOS}/milestone.json")
file(WRITE "${project}" "${goodProject}app_shell sideways\n")
run(1 "app_shell takes off, all" --flow-test "${SCENARIOS}/milestone.json")
file(WRITE "${project}" "${goodProject}")

run(0 "flow test passed: 12 steps" --app-shell all --flow-test "${SCENARIOS}/refusals.json")

# The project's own menu screen replaces the built-in one; --check reads it with the
# shell's "flow" model.
file(MAKE_DIRECTORY "${root}/Content/Shell")
file(WRITE "${root}/Content/Shell/menu.rml"
    "<rml><head><style>body { font-family: \"Noto Sans\"; } button { display: block; }</style></head>\n"
    "<body data-model=\"flow\"><button id=\"new_game\" onclick=\"new_game\">Play {{title}}</button></body></rml>\n")
run(0 "ok Content/Shell/menu.rml" --check)
run(0 "flow test passed: 23 steps" --app-shell all --flow-test "${SCENARIOS}/milestone.json")
file(REMOVE_RECURSE "${root}/Content/Shell")

# Pause, then load a save made in the level.
run(0 "flow test passed: 7 steps" --app-shell all --flow-test "${SCENARIOS}/save-load.json")

# A failing step: exit 1, which step and why, and the state then.
run(1 "flow test failed: step 2 \\(assert_level b\\): level \"menu\" is playing" --app-shell all
    --flow-test "${SCENARIOS}/fails.json" --json "${TEMP}/fails.json")
expect("${TEMP}/fails.json" "\"failed_step\": 2")
expect("${TEMP}/fails.json" "\"screen\": \"menu\"")
# Bad scenarios and arguments are usage errors.
file(WRITE "${TEMP}/bad.json" "{\"format\": 1, \"steps\": [{\"wait_phase\": \"soon\"}]}")
run(2 "isn't a travel phase" --flow-test "${TEMP}/bad.json")
file(WRITE "${TEMP}/script.json" "{\"format\": 1, \"steps\": [{\"run\": \"rm -rf /\"}]}")
run(2 "it needs one of travel, click" --flow-test "${TEMP}/script.json")
run(2 "can't read" --flow-test "${TEMP}/missing.json")
run(2 "--app-shell: app_shell: unknown screen" --app-shell "menu shop" --flow-test "${SCENARIOS}/fails.json")
# Without the shell there's nothing to click.
file(WRITE "${TEMP}/click.json" "{\"format\": 1, \"steps\": [{\"click\": \"new_game\"}]}")
run(1 "there's no application shell" --app-shell off --flow-test "${TEMP}/click.json")

# A level that doesn't load: the error screen over the menu, Retry, Main menu.
file(WRITE "${root}/Scenes/a.swscene" "shockwave_scene 2\nentity \"Broken\"\n  Transform\n    position 0 0\nend\n")
run(0 "flow test passed: 12 steps" --app-shell all --flow-test "${SCENARIOS}/broken-level.json")
file(WRITE "${root}/Scenes/a.swscene" "${goodA}")
# A menu level that doesn't load: Main menu fails, and the level plays on behind it.
file(WRITE "${root}/Scenes/menu.swscene" "shockwave_scene 2\nentity \"Broken\"\n  Nonsense\nend\n")
run(0 "flow test passed: 12 steps" --app-shell all --level a --flow-test "${SCENARIOS}/broken-menu.json")
file(WRITE "${root}/Scenes/menu.swscene" "${goodMenu}")
