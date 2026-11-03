# The release contract's command-line paths (S00, docs/Release.md) on a copy of Arena:
#   - Arena as it is, without a contract, still checks clean and reports "release none";
#   - a good contract checks clean and --release-info names every gate, passed, failed or
#     unverified, as text and as JSON;
#   - mistakes fail --check and --release-info with file:line, and rules shared with the
#     project, package and network settings are checked against those files;
#   - --release-schema matches the checked-in docs/ReleaseSchema.md, and the tools run alone.
#   -DPLAYER=player -DPROJECT_DIR=Games/Arena -DSCHEMA=docs/ReleaseSchema.md -DTEMP=dir
cmake_policy(SET CMP0054 NEW)
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}")
file(COPY "${PROJECT_DIR}/" DESTINATION "${TEMP}/Arena" PATTERN "Saved" EXCLUDE)
set(project "${TEMP}/Arena/Arena.swproject")
set(contract "${TEMP}/Arena/Config/Release.swrelease")
set(network "${TEMP}/Arena/Config/Network.swnetwork")
set(package "${TEMP}/Arena/Config/Package.swpackage")
file(READ "${network}" goodNetwork)

function(run expected pattern)
    execute_process(COMMAND "${PLAYER}" --project "${project}" ${ARGN}
                    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL ${expected} OR NOT "${out}${err}" MATCHES "${pattern}")
        message(FATAL_ERROR "player ${ARGN}: expected exit ${expected} and '${pattern}', got ${result}:\n${out}\n${err}")
    endif()
    set(out "${out}" PARENT_SCOPE)
endfunction()

# ---- A legacy project: no contract, nothing changes ----
run(0 "0 problems" --check)
if(out MATCHES "Release.swrelease")
    message(FATAL_ERROR "--check mentions a release contract Arena doesn't have:\n${out}")
endif()
run(0 "release none.*gate release_contract unverified.*qualified no" --release-info)
run(0 "Wrote" --release-info --json "${TEMP}/none.json")
file(READ "${TEMP}/none.json" json)
string(JSON present GET "${json}" present)
string(JSON qualified GET "${json}" qualified)
string(JSON format GET "${json}" format)
if(NOT present STREQUAL "OFF" OR NOT qualified STREQUAL "OFF" OR NOT format EQUAL 1)
    message(FATAL_ERROR "--release-info --json without a contract: ${json}")
endif()

# ---- A good offline contract ----
set(good "shockwave_release 1
application_id \"com.shockwave.arena\"
release_version \"1.0.0\"
platform linux-x86_64
platform windows-x86_64
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
acceptance_scenario \"Tests/complete_game.json\"
")
file(WRITE "${contract}" "${good}")
run(0 "warning Config/Release.swrelease:16: acceptance_scenario.*ok Config/Release.swrelease.*0 problems" --check)
file(MAKE_DIRECTORY "${TEMP}/Arena/Tests")
file(WRITE "${TEMP}/Arena/Tests/complete_game.json" "{}")
run(0 "ok Config/Release.swrelease" --check)
run(0 "gate release_contract passed.*gate project_settings passed.*gate steam_app_id not_applicable.*gate user_paths passed \\(S01\\): installed, it writes under [^\n]*com.shockwave.arena.*gate platform:windows-x86_64 unverified \\(S13\\).*qualified no"
    --release-info)

# The contract's application_id names the per-user folders (S01, docs/UserPaths.md).
run(0 "application_id com.shockwave.arena" --user-paths)
run(0 "Wrote" --release-info --json "${TEMP}/good.json")
file(READ "${TEMP}/good.json" json)
string(JSON id GET "${json}" contract application_id)
string(JSON platforms LENGTH "${json}" contract platform)
string(JSON gates LENGTH "${json}" gates)
string(JSON first GET "${json}" gates 0 id)
string(JSON firstStatus GET "${json}" gates 0 status)
string(JSON failed GET "${json}" summary failed)
string(JSON unverified GET "${json}" summary unverified)
if(NOT id STREQUAL "com.shockwave.arena" OR NOT platforms EQUAL 2 OR NOT first STREQUAL "release_contract" OR
   NOT firstStatus STREQUAL "passed" OR NOT failed EQUAL 0 OR unverified LESS 10 OR gates LESS 14)
    message(FATAL_ERROR "--release-info --json is missing fields:\n${json}")
endif()
# Every gate is passed, failed, unverified or not_applicable, with a batch and a reason.
math(EXPR last "${gates} - 1")
foreach(i RANGE ${last})
    string(JSON status GET "${json}" gates ${i} status)
    string(JSON batch GET "${json}" gates ${i} batch)
    string(JSON reason GET "${json}" gates ${i} reason)
    if(NOT status MATCHES "^(passed|failed|unverified|not_applicable)$" OR NOT batch MATCHES "^S[0-9][0-9]$" OR
       reason STREQUAL "")
        message(FATAL_ERROR "gate ${i} is malformed: ${status} ${batch} ${reason}")
    endif()
endforeach()

# ---- Mistakes in the contract: file:line, nonzero exit ----
function(bad from to pattern)
    string(REPLACE "${from}" "${to}" text "${good}")
    if(text STREQUAL good)
        message(FATAL_ERROR "test bug: '${from}' isn't in the contract")
    endif()
    file(WRITE "${contract}" "${text}")
    run(1 "${pattern}" --check)
    run(1 "${pattern}" --release-info)
    file(WRITE "${contract}" "${good}")
endfunction()
bad("max_players 1" "max_players 3" "Config/Release.swrelease:7: max_players 3 needs mode online")
bad("target_fps 60" "target_fps nan" "Config/Release.swrelease:8: target_fps must be a whole number")
bad("transition_timeout_seconds 30" "transition_timeout_seconds nan" "Config/Release.swrelease:15: transition_timeout_seconds must be")
bad("cpu_budget_mib 4096" "cpu_budget_mib 99999999999999999999" "Config/Release.swrelease:11: cpu_budget_mib must be")
bad("platform windows-x86_64" "platform linux-x86_64" "Config/Release.swrelease:5: platform linux-x86_64 is listed twice")
bad("shared_budget_mib 0\n" "" "Config/Release.swrelease: missing shared_budget_mib")
bad("shockwave_release 1" "shockwave_release 7" "Config/Release.swrelease:1: unsupported release contract version 7")
bad("application_id \"com.shockwave.arena\"" "application_id \"Arena\""
    "Config/Release.swrelease:2: application_id \"Arena\"")
# Rules shared with the project and its other settings.
bad("simulation_hz 60" "simulation_hz 120" "Config/Release.swrelease:9: simulation_hz 120 doesn't match the project's tick_rate 60")
bad("release_version \"1.0.0\"" "release_version \"2.0.0\"" "release_version 2.0.0 doesn't match the package version 1.0.0")
file(WRITE "${package}" "shockwave_package 1\nversion \"2.0.0\"\n")
string(REPLACE "release_version \"1.0.0\"" "release_version \"2.0.0\"" text "${good}")
file(WRITE "${contract}" "${text}")
run(0 "ok Config/Release.swrelease" --check)
file(REMOVE "${package}")
file(WRITE "${contract}" "${good}")
file(APPEND "${TEMP}/Arena/Config/Graphics.swgraphics" "graphics_memory_budget_mb 256\n")
bad("gpu_budget_mib 2048" "gpu_budget_mib 128" "gpu_budget_mib 128 is less than the project's graphics_memory_budget_mb 256")

# ---- Online: the network settings must agree ----
set(online "${good}")
string(REPLACE "mode offline" "mode offline\nmode online" online "${online}")
string(REPLACE "max_players 1\n" "max_players 12\n" online "${online}")
file(WRITE "${contract}" "${online}")
run(1 "mode online, but Config/Network.swnetwork has multiplayer off" --check)
string(REPLACE "enabled 0" "enabled 1" text "${goodNetwork}")
string(REPLACE "steam_app_id 0" "steam_app_id 480" text "${text}")
file(WRITE "${network}" "${text}")
run(0 "warning Config/Release.swrelease:6: Config/Network.swnetwork plays as Steam AppID 480" --check)
run(0 "gate steam_app_id failed.*gate online_lifecycle unverified \\(S11\\)" --release-info)
string(REPLACE "steam_app_id 480" "steam_app_id 1234560" text "${text}")
file(WRITE "${network}" "${text}")
run(0 "gate steam_app_id passed" --release-info)
string(REPLACE "max_players 12" "max_players 16" mismatched "${text}")
file(WRITE "${network}" "${mismatched}")
run(1 "Config/Release.swrelease:8: max_players 12 doesn't match Config/Network.swnetwork's max_players 16" --check)
# Offline only while the network settings turn multiplayer on.
file(WRITE "${contract}" "${good}")
run(1 "Config/Network.swnetwork turns multiplayer on, but the contract has no mode online" --release-info)
file(WRITE "${network}" "${goodNetwork}")

# ---- The schema, and modes that run alone ----
execute_process(COMMAND "${PLAYER}" --release-schema - RESULT_VARIABLE result OUTPUT_VARIABLE generated ERROR_VARIABLE err)
file(READ "${SCHEMA}" checkedIn)
string(REPLACE "\r\n" "\n" checkedIn "${checkedIn}")
if(NOT result EQUAL 0 OR NOT generated STREQUAL checkedIn)
    message(FATAL_ERROR "docs/ReleaseSchema.md is out of date; run: player --release-schema docs/ReleaseSchema.md\n${err}")
endif()
run(2 "can't be combined" --release-info --network-info)
run(2 "can't be combined" --release-info --package)
message(STATUS "release contract: legacy, good, bad and shared-rule cases behave")
