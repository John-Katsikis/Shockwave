# player --perf-run (S10, docs/Performance.md) on a copy of Reactor Run, then the release's
# performance gate reading the records it writes.
#   -DPLAYER=player -DPROJECT=Games/ShippingDemo -DTEMP=dir
#
# The tour plays menu -> training -> factory -> menu twice in real time under the null
# renderer. The report keeps loading, warm-up and play apart, per level, with memory over
# the laps; --perf-record writes a record. That record (null renderer) never counts; the
# same record from real hardware, on an optimised build within every budget, passes the
# gate; one budget missed fails it. CI proves the accounting, never the speed.
cmake_policy(SET CMP0057 NEW)
foreach(var PLAYER PROJECT TEMP)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "PerformanceCheck.cmake needs -D${var}=...")
    endif()
endforeach()
file(REMOVE_RECURSE "${TEMP}")
set(G "${TEMP}/Reactor Run")
file(COPY "${PROJECT}/" DESTINATION "${G}" PATTERN "Saved" EXCLUDE)
set(headless SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy)

function(run expect)
    execute_process(COMMAND ${CMAKE_COMMAND} -E env ${headless} "${PLAYER}" --project "${G}" ${ARGN}
        WORKING_DIRECTORY "${TEMP}" RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL expect)
        string(REPLACE ";" " " shown "${ARGN}")
        message(FATAL_ERROR "${shown}: expected exit ${expect}, got ${code}:\n${out}\n${err}")
    endif()
    set(out_text "${out}${err}" PARENT_SCOPE)
endfunction()
function(expect_text pattern what)
    if(NOT out_text MATCHES "${pattern}")
        message(FATAL_ERROR "${what}: expected \"${pattern}\" in:\n${out_text}")
    endif()
endfunction()

# ---- Two laps of the tour, measured ----
run(0 --renderer null --user-dir "${TEMP}/user" --perf-run "${G}/Tests/perf/tour.json" --laps 2
    --json "${TEMP}/perf.json" --perf-record "CI null renderer" --output "${TEMP}/records")
expect_text("perf run: Tests/perf/tour.json, 2 laps, warm cache" "the run's header")
expect_text("play: +[0-9]+ frames, p50 [0-9.]+ ms, p95 [0-9.]+ ms, p99 [0-9.]+ ms, worst" "play percentiles")
expect_text("level training +[0-9]+ frames" "training's own frames")
expect_text("level factory +[0-9]+ frames" "factory's own frames")
expect_text("warm-up: +[0-9]+ frames.*apart" "the warm-up, reported apart")
expect_text("loading: +6 changes" "every level change, as loading")
expect_text("from the first lap's end to the last" "memory over the laps")
expect_text("flow test passed" "the tour itself")
file(READ "${TEMP}/perf.json" json)
string(JSON mode GET "${json}" mode)
string(JSON passed GET "${json}" passed)
string(JSON laps GET "${json}" laps)
string(JSON loads LENGTH "${json}" loads)
string(JSON levels LENGTH "${json}" levels)
string(JSON play GET "${json}" play frames)
string(JSON warmup GET "${json}" warmup frames)
string(JSON load0 GET "${json}" loads 0 level)
string(JSON flowOk GET "${json}" flow ok)
if(NOT mode STREQUAL "perf-run" OR NOT passed OR NOT laps EQUAL 2 OR NOT loads EQUAL 6 OR levels LESS 3 OR
   play LESS 10 OR warmup LESS 10 OR NOT load0 STREQUAL "training" OR NOT flowOk)
    message(FATAL_ERROR "the perf run's JSON is missing something:\n${json}")
endif()
set(record "${TEMP}/records/ci-null-renderer.swperf")
if(NOT EXISTS "${record}")
    message(FATAL_ERROR "--perf-record didn't write ${record}")
endif()
file(READ "${record}" recorded)
foreach(line "machine \"CI null renderer\"" "renderer null" "laps 2" "cache warm" "scenario \"Tests/perf/tour.json\"")
    string(FIND "${recorded}" "${line}" at)
    if(at LESS 0)
        message(FATAL_ERROR "the record lacks '${line}':\n${recorded}")
    endif()
endforeach()

# ---- The gate ----
# A version 2 contract for Reactor Run, at 60 fps on medium.
file(WRITE "${G}/Config/Release.swrelease" "shockwave_release 2
application_id \"com.shockwave.reactorrun\"
release_version \"1.0.0\"
platform linux-x86_64
mode offline
max_players 1
target_fps 60
simulation_hz 60
graphics_tier medium
cpu_budget_mib 4096
gpu_budget_mib 2048
shared_budget_mib 0
loading_budget_mib 512
transition_timeout_seconds 30
acceptance_scenario \"Tests/flow/full-run.json\"
frame_p95_ms 16.667
frame_p99_ms 20.833
max_hitch_ms 50
level_load_seconds 10
")
run(0 --release-info)
expect_text("gate performance unverified \\(S10\\): no performance record yet" "no records yet")
# The CI record: the null renderer draws nothing, so it never counts.
file(MAKE_DIRECTORY "${G}/Release/Performance")
file(COPY "${record}" DESTINATION "${G}/Release/Performance")
run(0 --release-info)
expect_text("gate performance unverified \\(S10\\): no record counts yet.*null renderer, which draws nothing" "a null-renderer record")
# The same run as real hardware would report it, within every budget.
function(as_hardware text p95 out)
    string(REGEX REPLACE "renderer [a-z]+\n" "renderer vulkan\n" text "${text}")
    string(REGEX REPLACE "gpu \"[^\"]*\"\n" "gpu \"GeForce GTX 1060\"\n" text "${text}")
    string(REGEX REPLACE "gpu_kind [a-z]+\n" "gpu_kind dedicated\n" text "${text}")
    string(REGEX REPLACE "build_config [^\n]*\n" "build_config Release\n" text "${text}")
    string(REGEX REPLACE "build_id [^\n]*\n" "build_id \"\"\n" text "${text}")
    string(REGEX REPLACE "tier [a-z]+\n" "tier medium\n" text "${text}")
    string(REGEX REPLACE "resolution [0-9]+ [0-9]+\n" "resolution 1920 1080\n" text "${text}")
    string(REGEX REPLACE "frame_limit [^\n]*\n" "frame_limit 0\n" text "${text}")
    string(REGEX REPLACE "\nplay [^\n]*\n" "\nplay 3000 9.5 ${p95} 18 31 10\n" text "${text}")
    string(REGEX REPLACE "cpu_peak_mib [0-9]+\n" "cpu_peak_mib 1500\n" text "${text}")
    string(REGEX REPLACE "gpu_peak_mib [0-9]+\n" "gpu_peak_mib 900\n" text "${text}")
    string(REGEX REPLACE "memory_growth_mib -?[0-9]+\n" "memory_growth_mib 0\n" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()
as_hardware("${recorded}" 14 fast)
file(WRITE "${G}/Release/Performance/minimum-pc.swperf" "${fast}")
run(0 --release-info)
expect_text("gate performance passed \\(S10\\): CI null renderer \\(Release/Performance/minimum-pc.swperf\\): GeForce GTX 1060, 1920x1080 at medium.*every budget met" "a record within every budget")
# One budget missed: the gate fails and says which.
as_hardware("${recorded}" 17.5 slow)
file(WRITE "${G}/Release/Performance/minimum-pc.swperf" "${slow}")
run(0 --release-info)
expect_text("gate performance failed \\(S10\\): .*frame p95 17.5 ms is over the budget 16.67 ms" "a record over budget")
# A damaged record is a warning, never a crash or a silent pass.
file(WRITE "${G}/Release/Performance/broken.swperf" "shockwave_performance 1\nfps 60\n")
run(0 --release-info)
expect_text("warning Release/Performance/broken.swperf:2: unknown key fps" "a damaged record")
# The editor's Release readiness and the JSON report carry the same gate.
run(0 --release-info --json "${TEMP}/release.json")
file(READ "${TEMP}/release.json" release)
string(JSON count LENGTH "${release}" gates)
math(EXPR last "${count} - 1")
set(found "")
foreach(i RANGE ${last})
    string(JSON id GET "${release}" gates ${i} id)
    if(id STREQUAL "performance")
        string(JSON found GET "${release}" gates ${i} status)
    endif()
endforeach()
string(JSON p95 GET "${release}" contract frame_p95_ms)
if(NOT found STREQUAL "failed" OR NOT p95 EQUAL 16.667)
    message(FATAL_ERROR "--release-info --json: performance ${found}, frame_p95_ms ${p95}:\n${release}")
endif()

# ---- What goes with what ----
run(2 --laps 2)
expect_text("--laps and --perf-record go with --perf-run" "--laps alone")
run(2 --perf-run "${G}/Tests/perf/tour.json" --flow-test "${G}/Tests/perf/tour.json")
expect_text("--perf-run plays its own scenario" "--perf-run with --flow-test")
run(2 --perf-run "${G}/Tests/perf/tour.json" --laps 0)
run(2 --cold-cache)
message(STATUS "performance: perf run, records and the gate behave")
