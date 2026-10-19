# The multiplayer command-line paths on a copy of Arena (docs/Networking.md): --check's
# network rules with file:line, --network-info (text and JSON), --network-schema matching
# the checked-in docs/NetworkSchema.md, and modes that can't be combined. None of them
# open a socket. --steam-status is the one that would start Steam: it's only run where it
# fails without doing so (no AppID, or a build without the Steamworks SDK).
#   -DPLAYER=player -DPROJECT_DIR=Games/Arena -DSCHEMA=docs/NetworkSchema.md -DTEMP=dir [-DSTEAMWORKS=ON]
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}")
file(COPY "${PROJECT_DIR}/" DESTINATION "${TEMP}/Arena" PATTERN "Saved" EXCLUDE)
set(project "${TEMP}/Arena/Arena.swproject")
set(settings "${TEMP}/Arena/Config/Network.swnetwork")
set(manifest "${TEMP}/Arena/Config/NetworkManifest.swnetmanifest")
set(scene "${TEMP}/Arena/Scenes/arena.swscene")
file(READ "${settings}" goodSettings)
file(READ "${manifest}" goodManifest)
file(READ "${scene}" goodScene)

function(run expected pattern)
    execute_process(COMMAND "${PLAYER}" --project "${project}" ${ARGN}
                    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL ${expected} OR NOT "${out}${err}" MATCHES "${pattern}")
        message(FATAL_ERROR "player ${ARGN}: expected exit ${expected} and '${pattern}', got ${result}:\n${out}\n${err}")
    endif()
endfunction()

# Settings that differ from the good file by one replacement fail --check with file:line.
function(bad_settings from to pattern)
    string(REPLACE "${from}" "${to}" text "${goodSettings}")
    file(WRITE "${settings}" "${text}")
    run(1 "${pattern}" --check)
    file(WRITE "${settings}" "${goodSettings}")
endfunction()

run(0 "ok Config/Network.swnetwork" --check)
run(0 "schema digest [0-9a-f]+" --network-info)
run(0 "content digest [0-9a-f]+" --network-info)
run(0 "Wrote" --network-info --json "${TEMP}/info.json")
file(READ "${TEMP}/info.json" json)
if(NOT json MATCHES "\"steam_app_id\": \"0\"" OR NOT json MATCHES "\"schema\": \"[0-9a-f]+\"")
    message(FATAL_ERROR "--network-info --json is missing fields:\n${json}")
endif()

# The checked-in schema is what the build generates.
execute_process(COMMAND "${PLAYER}" --project "${project}" --network-schema -
                RESULT_VARIABLE result OUTPUT_VARIABLE generated ERROR_VARIABLE err)
file(READ "${SCHEMA}" checkedIn)
string(REPLACE "\r\n" "\n" checkedIn "${checkedIn}")
if(NOT result EQUAL 0 OR NOT generated STREQUAL checkedIn)
    message(FATAL_ERROR "docs/NetworkSchema.md is out of date; run: player --project Games/Arena/Arena.swproject "
                        "--network-schema docs/NetworkSchema.md\n${err}")
endif()

# Overrides: tick_rate 60..120 and max_players 2..64; ranges are errors, never clamped.
bad_settings("max_players 12" "max_players 65" "Network.swnetwork:[0-9]+: max_players must be a whole number from 2 to 64")
bad_settings("max_players 12" "max_players 1" "max_players must be a whole number from 2 to 64")
bad_settings("tick_rate 60" "tick_rate 130" "Network.swnetwork:[0-9]+: tick_rate must be a whole number from 60 to 120")
bad_settings("tick_rate 60" "tick_rate 120" "doesn't match the project's tick_rate 60")
bad_settings("snapshot_rate 20" "snapshot_rate 25" "snapshot_rate must be 10, 15, 20, 30 or 60")
bad_settings("enabled 0" "enabled 1" "steam_app_id is required")
bad_settings("max_players 12" "max_players 64" "64 players at snapshot_rate 20 need about [0-9]+ bytes/s")
bad_settings("max_players 12" "max_players 12\nbogus 1" "Network.swnetwork:[0-9]+: unknown setting bogus")
bad_settings("max_players 12" "max_players 12\nmax_players 13" "duplicate setting max_players")
bad_settings("enabled 0" "enabled 0\nconnect_timeout_seconds nan" "connect_timeout_seconds")
# 120 Hz is fine when the project runs at 120 too.
string(REPLACE "tick_rate 60" "tick_rate 120" text "${goodSettings}")
string(REPLACE "input_rate 60" "input_rate 120" text "${text}")
file(WRITE "${settings}" "${text}")
file(READ "${project}" goodProject)
string(REPLACE "tick_rate 60" "tick_rate 120" fast "${goodProject}")
file(WRITE "${project}" "${fast}")
run(0 "ok Config/Network.swnetwork" --check)
file(WRITE "${project}" "${goodProject}")
file(WRITE "${settings}" "${goodSettings}")

# The manifest.
file(WRITE "${manifest}" "${goodManifest}scene 1 \"Scenes/arena-benchmark.swscene\"\n")
run(1 "NetworkManifest.swnetmanifest:[0-9]+: scene ID 1 is already used" --check)
file(WRITE "${manifest}" "${goodManifest}scene 2 \"Scenes/missing.swscene\"\n")
run(1 "scene Scenes/missing.swscene doesn't exist" --check)
file(WRITE "${manifest}" "${goodManifest}prefab 1 \"Prefabs/Arena bot 0.swprefab\" \"arena.player.v1\"\n")
run(1 "its root has no NetworkObject" --check)
file(WRITE "${manifest}" "${goodManifest}bogus 1\n")
run(1 "unknown manifest entry bogus" --check)
file(WRITE "${manifest}" "${goodManifest}")

# A scene's network objects.
file(WRITE "${scene}" "${goodScene}
entity \"Net door\"
  Transform
  NetworkObject
    key \"door\"
    profile \"arena.missing.v1\"
end

entity \"Net door copy\"
  Transform
  NetworkObject
    key \"door\"
    profile \"engine.pose.v1\"
end
")
run(1 "arena.swscene: entity 'Net door': NetworkObject profile \"arena.missing.v1\" isn't registered" --check)
run(1 "entity 'Net door copy': NetworkObject key \"door\" is also used by 'Net door'" --check)
file(WRITE "${scene}" "${goodScene}")

# --steam-status needs an AppID, and a build with the SDK; it fails visibly otherwise.
run(1 "steam_app_id is 0 in Config/Network.swnetwork" --steam-status)
if(NOT STEAMWORKS)
    string(REPLACE "steam_app_id 0" "steam_app_id 480" text "${goodSettings}")
    file(WRITE "${settings}" "${text}")
    run(1 "build steamworks no.*steam unavailable: this build has no Steamworks SDK" --steam-status)
    run(1 "steam unavailable" --steam-status server)
    file(WRITE "${settings}" "${goodSettings}")
endif()

# The tools run on their own.
run(2 "run on their own" --network-info --check)
run(2 "run on their own" --network-info --network-schema -)
run(2 "run on their own" --steam-status --check)
run(2 "run on their own" --steam-status --network-info)
run(0 "0 problems" --check)
file(REMOVE_RECURSE "${TEMP}")
