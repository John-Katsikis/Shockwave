# The machine-run steps of the installed-game acceptance sequence (S16, docs/Acceptance.md,
# section 25 of the shipping plan) on Reactor Run, in a copy of the project.
#   -DPLAYER=player -DPROJECT=Games/ShippingDemo -DTEMP=dir -DPART=round_trips|failures
#
# round_trips (step 10): menu, training, factory, menu 100 times; at each lap's end the
#   entities, physics bodies, resident assets, leases and threads are back where lap 1 left
#   them and resident memory stays within the tolerance. A tolerance of 0 MiB with a growing
#   process is caught; --laps and --lap-tolerance refuse what they can't do.
# failures (step 11): a full disk fails the save with WriteFailed and the last save stays; a
#   change cancelled at the boundary leaves training playing; a missing and a corrupt level
#   fail with LoadFailed, the error screen's Main menu recovers, Continue loads the save.
#   A level change slower than --transition-timeout fails as LoadTimeout.
foreach(var PLAYER PROJECT TEMP PART)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "AcceptanceCheck.cmake needs -D${var}=...")
    endif()
endforeach()
file(REMOVE_RECURSE "${TEMP}")
set(G "${TEMP}/Reactor Run")
file(COPY "${PROJECT}/" DESTINATION "${G}" PATTERN "Saved" EXCLUDE PATTERN "Release" EXCLUDE)
set(FLOWS "${PROJECT}/Tests/flow")
set(headless SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy)

function(run expect)
    execute_process(COMMAND ${CMAKE_COMMAND} -E env ${headless} "${PLAYER}" --project "${G}" --renderer null ${ARGN}
        WORKING_DIRECTORY "${TEMP}" RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL expect)
        string(REPLACE ";" " " shown "${ARGN}")
        message(FATAL_ERROR "player ${shown}: expected exit ${expect}, got ${code}:\n${out}\n${err}")
    endif()
    set(out_text "${out}${err}" PARENT_SCOPE)
endfunction()
function(expect_text pattern what)
    if(NOT out_text MATCHES "${pattern}")
        message(FATAL_ERROR "${what}: expected \"${pattern}\" in:\n${out_text}")
    endif()
endfunction()

if(PART STREQUAL "round_trips")
    run(0 --flow-test "${FLOWS}/round-trip.json" --laps 100 --json "${TEMP}/laps.json")
    expect_text("laps: 100; lap 1 ended with [0-9.]+ MiB" "the watermarks")
    expect_text("flow test passed: 901 steps" "100 laps")
    file(READ "${TEMP}/laps.json" json)
    string(JSON plateau GET "${json}" run plateau)
    string(JSON laps LENGTH "${json}" run laps)
    string(JSON first_entities GET "${json}" run laps 0 entities)
    string(JSON last_entities GET "${json}" run laps 99 entities)
    string(JSON last_leases GET "${json}" run laps 99 leases)
    if(NOT plateau OR NOT laps EQUAL 100 OR NOT first_entities EQUAL last_entities OR last_leases LESS 1)
        message(FATAL_ERROR "the round trips' report: ${json}")
    endif()
    # A lap that doesn't end where it began is refused, not measured.
    file(WRITE "${TEMP}/one-way.json" "{\"format\": 1, \"steps\": [{\"travel\": \"training\"},
        {\"wait_phase\": \"succeeded\", \"timeout\": 30}, {\"quit\": true}]}")
    run(1 --flow-test "${TEMP}/one-way.json" --laps 3)
    expect_text("flow test failed: lap 1 ended in training, not menu where it began" "a lap that doesn't come back")
    # What the options refuse.
    run(2 --laps 3)
    expect_text("--laps goes with --flow-test or --perf-run" "--laps alone")
    run(2 --flow-test "${FLOWS}/round-trip.json" --laps 2 --lap-tolerance 8)
    expect_text("--lap-tolerance goes with --flow-test and --laps 3 or more" "a tolerance for two laps")
    run(2 --flow-test "${FLOWS}/round-trip.json" --laps 3 --lap-tolerance lots)
    expect_text("--lap-tolerance takes MiB" "a tolerance that isn't a number")
elseif(PART STREQUAL "failures")
    file(REMOVE "${G}/Scenes/factory.swscene")
    file(WRITE "${G}/Scenes/ending.swscene" "not a scene {{{\n")
    run(0 --flow-test "${FLOWS}/failures.json" --json "${TEMP}/failures.json")
    expect_text("factory.swscene" "the missing level named")
    expect_text("ending.swscene.*not a Shockwave scene" "the corrupt level named")
    expect_text("flow test passed: 30 steps" "every injected failure recovered")
    # The save written before the full disk is still the one that loads.
    file(GLOB_RECURSE saves "${G}/Saved/*acceptance.swsave")
    list(LENGTH saves count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "the acceptance save: ${saves}")
    endif()
    file(GLOB_RECURSE temporary "${G}/Saved/*.tmp*")
    if(temporary)
        message(FATAL_ERROR "the failed save left temporary files: ${temporary}")
    endif()
    # The transition timeout (S16): the player wires the contract's (else 120 s) and the flag.
    file(WRITE "${TEMP}/slow.json" "{\"format\": 1, \"steps\": [{\"travel\": \"training\"},
        {\"wait_phase\": \"failed\", \"expect\": \"LoadTimeout\", \"timeout\": 30}, {\"assert_level\": \"menu\"},
        {\"quit\": true}]}")
    run(0 --flow-test "${TEMP}/slow.json" --transition-timeout 0.000001)
    expect_text("took longer than 1e-06 s to load \\(transition_timeout_seconds\\)" "a change past the timeout")
    run(1 --flow-test "${TEMP}/slow.json" --transition-timeout 0)
    expect_text("ended succeeded|ticket .* to \"training\" ended" "no timeout when it's off")
    run(2 --flow-test "${TEMP}/slow.json" --transition-timeout soon)
    expect_text("--transition-timeout takes seconds" "a timeout that isn't a number")
else()
    message(FATAL_ERROR "AcceptanceCheck.cmake: PART is round_trips or failures, not ${PART}")
endif()
