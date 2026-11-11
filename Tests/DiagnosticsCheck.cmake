# Diagnostics from the command line (S12, docs/Diagnostics.md): real crashes in a child
# program write reports with context, frames and the recent log (secrets redacted); the
# player names their frames (also from kept symbols after stripping), sees the unclean exit,
# keeps and rotates log files for a game run, removes its session marker on a clean exit,
# starts in safe mode, and logs an authentication failure without the ticket.
#
#   cmake -DPLAYER=player -DCRASH=crash_program -DPROJECT=Sample.swproject -DARENA=Arena.swproject
#         -DTEMP=dir -P DiagnosticsCheck.cmake
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}")

# The text in variable `var` must (or, with NOT, must not) match `pattern`.
function(has var pattern)
    if(NOT "${${var}}" MATCHES "${pattern}")
        message(FATAL_ERROR "expected '${pattern}' in:\n${${var}}")
    endif()
endfunction()
function(lacks var pattern)
    if("${${var}}" MATCHES "${pattern}")
        message(FATAL_ERROR "didn't expect '${pattern}' in:\n${${var}}")
    endif()
endfunction()
function(exists path)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "missing: ${path}")
    endif()
endfunction()
function(absent path)
    if(EXISTS "${path}")
        message(FATAL_ERROR "shouldn't exist: ${path}")
    endif()
endfunction()

# Runs the player; the result must be `expected` and stdout+stderr must match `pattern`.
function(run expected pattern)
    execute_process(COMMAND "${PLAYER}" ${ARGN} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code STREQUAL "${expected}" OR NOT "${out}${err}" MATCHES "${pattern}")
        message(FATAL_ERROR "player ${ARGN}\nexpected ${expected} matching '${pattern}', got ${code}:\n${out}${err}")
    endif()
    set(last "${out}${err}" PARENT_SCOPE)
endfunction()

# Crashes the program in `kind` and returns the report it wrote.
function(crash kind crashes logs program out_report)
    execute_process(COMMAND "${CRASH}" "${crashes}" ${kind} "${logs}" ${program}
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(code EQUAL 0 OR code EQUAL 2 OR code EQUAL 3)
        message(FATAL_ERROR "crash_program ${kind} didn't crash (${code}):\n${out}${err}")
    endif()
    if(NOT err MATCHES "${program} crashed \\(")
        message(FATAL_ERROR "crash_program ${kind}: no crash line on stderr:\n${err}")
    endif()
    file(GLOB reports "${crashes}/${program}-*.crash.txt")
    list(LENGTH reports count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "crash_program ${kind}: expected one report in ${crashes}, found ${count}")
    endif()
    set(${out_report} "${reports}" PARENT_SCOPE)
endfunction()

# ---- A segfault -----------------------------------------------------------------------------
crash(segfault "${TEMP}/seg/Crashes" "${TEMP}/seg/Logs" crash_program report)
file(READ "${report}" text)
foreach(part "shockwave_crash 1" "program crash_program" "signal SIGSEGV" "level: factory" "level_epoch: 3"
             "\\[frames\\]" "\\[log\\]" "Loading the factory" "ticket \\[redacted\\]" "\\[end\\]")
    has(text "${part}")
endforeach()
string(REPEAT "a" 128 ticket)
lacks(text "${ticket}")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    has(text "\\[modules\\]")
    run(0 "crashed: SIGSEGV.*#0  shockwaveTestCrashHere.*#1  shockwaveTestCrashOuter.*#2  main" --symbolize "${report}")
    run(0 "Wrote" --symbolize "${report}" --json "${TEMP}/symbolized.json")
    file(READ "${TEMP}/symbolized.json" json)
    has(json "\"mode\": \"symbolize\"")
    has(json "\"function\": \"shockwaveTestCrashHere")
endif()
file(WRITE "${TEMP}/not-a-report.txt" "hello\n")
run(1 "isn't a Shockwave crash report" --symbolize "${TEMP}/not-a-report.txt")
run(1 "couldn't open" --symbolize "${TEMP}/missing.crash.txt")

# ---- abort() and a trap ---------------------------------------------------------------------
crash(abort "${TEMP}/abort/Crashes" "${TEMP}/abort/Logs" crash_program report)
file(READ "${report}" text)
has(text "signal SIGABRT")
crash(trap "${TEMP}/trap/Crashes" "${TEMP}/trap/Logs" crash_program report)
file(READ "${report}" text)
has(text "signal SIG(ILL|TRAP)")

# ---- Symbols kept beside a stripped binary ---------------------------------------------------
set(CRASH_ORIGINAL "${CRASH}")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    find_program(STRIP strip)
    find_program(READELF readelf)
    if(STRIP AND READELF)
        execute_process(COMMAND "${READELF}" -n "${CRASH}" OUTPUT_VARIABLE notes)
        string(REGEX MATCH "Build ID: ([0-9a-f]+)" found "${notes}")
        set(id "${CMAKE_MATCH_1}")
        if(id STREQUAL "")
            message(FATAL_ERROR "crash_program has no GNU build ID:\n${notes}")
        endif()
        # Beside the original, so it finds the engine's libraries the same way; the kept copy
        # has the same file name, as the packager keeps them.
        get_filename_component(bin "${CRASH}" DIRECTORY)
        set(stripped "${bin}/crash_program_stripped")
        file(MAKE_DIRECTORY "${TEMP}/symbols/${id}")
        configure_file("${CRASH}" "${TEMP}/symbols/${id}/crash_program_stripped" COPYONLY)
        configure_file("${CRASH}" "${stripped}" COPYONLY)
        execute_process(COMMAND "${STRIP}" --strip-unneeded "${stripped}" RESULT_VARIABLE code)
        if(NOT code EQUAL 0)
            message(FATAL_ERROR "strip failed")
        endif()
        set(CRASH "${stripped}")
        crash(segfault "${TEMP}/strip/Crashes" "${TEMP}/strip/Logs" crash_program report)
        run(0 "crashed" --symbolize "${report}")
        lacks(last "shockwaveTestCrashHere")
        run(0 "#0  shockwaveTestCrashHere" --symbolize "${report}" --symbols "${TEMP}/nothing" --symbols "${TEMP}/symbols")
        file(REMOVE "${stripped}")
        set(CRASH "${CRASH_ORIGINAL}")
    endif()
endif()

# ---- The player sees the unclean exit -------------------------------------------------------
crash(segfault "${TEMP}/user/Crashes" "${TEMP}/user/Logs" player report)
run(0 "previous run unclean \\(started [0-9T:-]+Z, level factory\\).*previous run's report .*player-.*crash reports 1.*advice: start with --safe-mode"
    --project "${PROJECT}" --diagnostics --user-dir "${TEMP}/user")
run(0 "Wrote" --project "${PROJECT}" --diagnostics --user-dir "${TEMP}/user" --json "${TEMP}/diagnostics.json")
file(READ "${TEMP}/diagnostics.json" json)
foreach(part "\"mode\": \"diagnostics\"" "\"state\": \"unclean\"" "\"level\": \"factory\"" "\"signal\": \"SIGSEGV \\(11\\)\""
             "\"crash_capture\": true")
    has(json "${part}")
endforeach()
# --diagnostics only reads: the marker is still there.
exists("${TEMP}/user/Logs/player.session")

# A game run: it warns about the last run, keeps a log, and ends cleanly (marker gone).
run(0 "didn't end cleanly.*--safe-mode" --project "${PROJECT}" --user-dir "${TEMP}/user" --renderer null --frames 5)
absent("${TEMP}/user/Logs/player.session")
exists("${TEMP}/user/Logs/player.log")
file(READ "${TEMP}/user/Logs/player.log" log)
has(log "didn't end cleanly")
run(0 "previous run clean" --project "${PROJECT}" --diagnostics --user-dir "${TEMP}/user")
# The next run rotates the log; no crash folder is made by a run that doesn't crash.
run(0 "" --project "${PROJECT}" --user-dir "${TEMP}/fresh" --renderer null --frames 3)
run(0 "" --project "${PROJECT}" --user-dir "${TEMP}/fresh" --renderer null --frames 3)
exists("${TEMP}/fresh/Logs/player.1.log")
absent("${TEMP}/fresh/Crashes")
# --no-log-file, and tools keep no log.
run(0 "" --project "${PROJECT}" --user-dir "${TEMP}/nolog" --renderer null --frames 3 --no-log-file)
absent("${TEMP}/nolog/Logs/player.log")
run(0 "" --project "${PROJECT}" --user-dir "${TEMP}/tool" --check)
absent("${TEMP}/tool/Logs")

# ---- Safe mode -----------------------------------------------------------------------------
run(0 "Safe mode: lowest graphics, windowed, no pipeline cache" --project "${PROJECT}" --user-dir "${TEMP}/safe"
    --renderer null --frames 3 --safe-mode)

# ---- An authentication failure keeps no secret ------------------------------------------------
run(0 "ok" --project "${ARENA}" --net-test --scenario rejections --log-file "${TEMP}/net.log")
file(READ "${TEMP}/net.log" net)
has(net "AuthenticationFailed|AuthUnavailable|refused")
lacks(net "SWLT")
string(REPEAT "[0-9a-fA-F]" 96 longHex)
lacks(net "${longHex}")

# ---- Usage ----------------------------------------------------------------------------------
run(2 "--diagnostics.*--net-test|--net-test.*--diagnostics" --project "${PROJECT}" --diagnostics --net-test)
message(STATUS "diagnostics_cli: all checks passed")
