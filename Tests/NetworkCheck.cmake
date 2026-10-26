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
if(NOT json MATCHES "\"steam_app_id\": \"0\"" OR NOT json MATCHES "\"schema\": \"[0-9a-f]+\"" OR
   NOT json MATCHES "\"match_scene\": \"Scenes/arena-mp.swscene\"")
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

# And Arena's node reference, with its Network.Command.* and Network.Event.* nodes.
execute_process(COMMAND "${PLAYER}" --project "${project}" --renderer null --node-schema -
                RESULT_VARIABLE result OUTPUT_VARIABLE generated ERROR_VARIABLE err)
file(READ "${PROJECT_DIR}/Docs/CircuitNodes.md" checkedIn)
string(REPLACE "\r\n" "\n" checkedIn "${checkedIn}")
if(NOT result EQUAL 0 OR NOT generated STREQUAL checkedIn OR NOT generated MATCHES "Network.Event.arena.shot_resolved")
    message(FATAL_ERROR "Games/Arena/Docs/CircuitNodes.md is out of date; run: player --project "
                        "Games/Arena/Arena.swproject --node-schema Games/Arena/Docs/CircuitNodes.md\n${err}")
endif()

# Overrides: tick_rate 60..120 and max_players 2..64; ranges are errors, never clamped.
bad_settings("max_players 12" "max_players 65" "Network.swnetwork:[0-9]+: max_players must be a whole number from 2 to 64")
bad_settings("max_players 12" "max_players 1" "max_players must be a whole number from 2 to 64")
bad_settings("tick_rate 60" "tick_rate 130" "Network.swnetwork:[0-9]+: tick_rate must be a whole number from 60 to 120")
bad_settings("tick_rate 60" "tick_rate 120" "doesn't match the project's tick_rate 60")
bad_settings("snapshot_rate 20" "snapshot_rate 25" "snapshot_rate must be 10, 15, 20, 30, 60 or 120")
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

# Predicted characters collide with static bodies only: once the scene is a network scene
# (and the manifest's Net Player is a PredictedCharacter), its patrolling kinematic bots
# are errors that say so.
file(WRITE "${scene}" "${goodScene}
entity \"Net lamp\"
  Transform
  NetworkObject
    key \"lamp\"
    profile \"engine.pose.v1\"
    mode Static
end
")
run(1 "arena.swscene: entity 'Blue 1': a moving kinematic body can't share a scene with predicted characters in v1: they collide with static bodies only"
    --check)
file(WRITE "${scene}" "${goodScene}")

# Circuits in network play: each runs with only the nodes its execution allows. A
# Presentation circuit can't move things or ask the host; a LocalOwner one may ask.
file(WRITE "${TEMP}/Arena/Content/Circuits/net-move.swcircuit" "shockwave_circuit 1
node start Event.BeginPlay
node move Component.Transform.Set.position
  exec start.then
  in position = 0 1 0
")
file(WRITE "${TEMP}/Arena/Content/Circuits/net-ready.swcircuit" "shockwave_circuit 1
node start Event.BeginPlay
node ready Network.Command.arena.ready_to_play
  exec start.then
  in ready = 1
")
function(circuit_scene file execution)
    file(WRITE "${scene}" "${goodScene}
entity \"Net circuit\"
  Transform
  Circuit
    circuit \"Circuits/${file}\"
    execution ${execution}
end
")
endfunction()
circuit_scene(net-move.swcircuit Presentation)
run(1 "entity 'Net circuit': Circuit: Circuits/net-move.swcircuit:3: node move \\(Component.Transform.Set.position\\) changes gameplay, which a Presentation circuit can't do"
    --check)
circuit_scene(net-ready.swcircuit Presentation)
run(1 "net-ready.swcircuit:3: node ready \\(Network.Command.arena.ready_to_play\\) sends a request to the host" --check)
circuit_scene(net-ready.swcircuit LocalOwner)
run(0 "0 problems" --check)
circuit_scene(net-move.swcircuit Authority)
run(0 "0 problems" --check)
file(WRITE "${scene}" "${goodScene}")
file(REMOVE "${TEMP}/Arena/Content/Circuits/net-move.swcircuit" "${TEMP}/Arena/Content/Circuits/net-ready.swcircuit")

# --steam-status needs an AppID, and a build with the SDK; it fails visibly otherwise.
run(1 "steam_app_id is 0 in Config/Network.swnetwork" --steam-status)
if(NOT STEAMWORKS)
    string(REPLACE "steam_app_id 0" "steam_app_id 480" text "${goodSettings}")
    file(WRITE "${settings}" "${text}")
    run(1 "build steamworks no.*steam unavailable: this build has no Steamworks SDK" --steam-status)
    run(1 "steam unavailable" --steam-status server)
    file(WRITE "${settings}" "${goodSettings}")
endif()

# --net-test: the session handshake over the in-process loopback hub, with the
# project's settings. No Steam, socket or window.
run(0 "net-test handshake seed 7 preset cafe: ok.*peer client 3: client, active" --net-test --seed 7)
run(0 "net-test handshake seed 3 preset hostile: ok" --net-test --seed 3 --preset hostile --clients 11)
run(0 "peer ticket never answered: client, failed AuthUnavailable.*0 packets in flight, 0 auth sessions, 0 tickets"
    --net-test --scenario rejections)
run(0 "rounds 100" --net-test --scenario stop-start --json "${TEMP}/net-test.json")
# Replication: a moving box and spawned crates; every client (one late) ends with the host's state.
run(0 "net-test replication seed 5 preset poor: ok.*late\\): client, active.*max position error 0.000000 m"
    --net-test --scenario replication --seed 5 --preset poor --clients 5)
file(READ "${TEMP}/net-test.json" json)
if(NOT json MATCHES "\"mode\": \"net-test\"" OR NOT json MATCHES "\"ok\": true" OR NOT json MATCHES "\"rounds\": 100")
    message(FATAL_ERROR "--net-test --json is missing fields:\n${json}")
endif()
run(0 "net-test prediction seed 2 preset hostile: ok.*prediction: [0-9]+ ticks predicted.*final error 0.0000 m"
    --net-test --scenario prediction --seed 2 --preset hostile --clients 4)
run(2 "unknown scenario bogus" --net-test --scenario bogus)
run(2 "unknown fault preset bogus" --net-test --preset bogus)
run(2 "--clients must be 1 to max_players - 1" --net-test --clients 12)

# The tools run on their own.
run(2 "run on their own" --net-test --network-info)
run(2 "run on their own" --net-test --check)
run(2 "--check and --net-preview can.t be combined" --net-preview --check)
run(2 "--net-test and --net-preview can.t be combined" --net-preview cafe --net-test)
run(2 "run on their own" --network-info --check)
run(2 "run on their own" --network-info --network-schema -)
run(2 "run on their own" --steam-status --check)
run(2 "run on their own" --steam-status --network-info)

# Batch 9: online play from the command line, and the capture and authoring tools.
# Invalid arguments exit 2 and say what's wrong; nothing starts.
run(2 "--host, --join-lobby and --join-steam are one each" --host --join-lobby 5)
run(2 "--dev-package, --no-build and --no-archive go with --package" --dev-package)
run(2 "--join-steam is a developer direct join: add --dev-direct-join" --join-steam 123)
run(2 "--dev-direct-join goes with --host or --join-steam" --dev-direct-join)
run(2 "--dev-direct-join goes with --host or --join-steam" --join-lobby 5 --dev-direct-join)
run(2 "--join-lobby takes a decimal Steam ID" --join-lobby abc)
run(2 "--join-lobby takes a decimal Steam ID" --join-lobby 0)
run(2 "--join-steam takes a decimal Steam ID" --join-steam 99999999999999999999999 --dev-direct-join)
run(2 "\\+connect_lobby takes a decimal Steam ID" +connect_lobby -1)
run(2 "\\+connect_lobby is a join of its own" --host +connect_lobby 5)
run(2 "--net-capture records a session" --net-capture "${TEMP}/x.swnetcap")
run(2 "--check and --host can't be combined" --host --check)
run(2 "--package and --join-lobby can't be combined" --package --join-lobby 5)
run(2 "--net-test and --host can't be combined" --net-test --host)
run(2 "--screenshot and --join-steam can't be combined" --screenshot x.png --join-steam 5 --dev-direct-join)
run(2 "online play doesn't go with --load or --net-preview" --host --load slot)
run(2 "--net-encode needs --output" --net-encode x.json)
run(2 "--output goes with --package, --net-encode" --output x)
run(2 "--network-manifest needs --output" --network-manifest)
run(2 "--assign-network-keys needs --scene file.swscene and --output" --assign-network-keys --scene "${scene}")
run(2 "Unknown or incomplete option: --bogus" --bogus)
# Hosting needs multiplayer on, then Steam: each fails visibly, with exit 1.
run(1 "can't play online: Config/Network.swnetwork: multiplayer isn't enabled" --host --renderer null)
if(NOT STEAMWORKS)
    # A Steam build would start the real Steam here; CTest never does.
    string(REPLACE "enabled 0" "enabled 1" text "${goodSettings}")
    string(REPLACE "steam_app_id 0" "steam_app_id 480" text "${text}")
    file(WRITE "${settings}" "${text}")
    run(1 "can't host: this build has no Steamworks SDK" --host --renderer null)
    run(1 "can't join: this build has no Steamworks SDK" --join-lobby 109775240000000000 --renderer null)
    run(1 "can't host: this build has no Steamworks SDK" --host --dev-direct-join --renderer null)
    file(WRITE "${settings}" "${goodSettings}")
endif()
string(REPLACE "match_scene \"Scenes/arena-mp.swscene\"" "match_scene \"Scenes/missing.swscene\"" text "${goodSettings}")
file(WRITE "${settings}" "${text}")
run(1 "Network.swnetwork:[0-9]+: match_scene \"Scenes/missing.swscene\" isn't a scene in Config/NetworkManifest.swnetmanifest" --check)
file(WRITE "${settings}" "${goodSettings}")

# --net-capture on a preview session, decoded by --net-dump and encoded back byte for byte.
run(0 "net capture: [1-9][0-9]* records" --scene "${TEMP}/Arena/Scenes/arena-mp.swscene" --net-preview clean
    --net-capture "${TEMP}/preview.swnetcap" --frames 120 --renderer null)
run(0 "Wrote" --net-dump "${TEMP}/preview.swnetcap" --json "${TEMP}/preview.json")
file(READ "${TEMP}/preview.json" json)
if(NOT json MATCHES "\"message\": \"Hello\"" OR NOT json MATCHES "\"redacted\": true")
    message(FATAL_ERROR "--net-dump: expected a decoded Hello and a redacted ticket:\n${json}")
endif()
run(0 "Wrote .*preview-again.swnetcap" --net-encode "${TEMP}/preview.json" --output "${TEMP}/preview-again.swnetcap")
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${TEMP}/preview.swnetcap" "${TEMP}/preview-again.swnetcap"
                RESULT_VARIABLE same)
if(NOT same EQUAL 0)
    message(FATAL_ERROR "--net-dump then --net-encode didn't give back the same capture")
endif()
string(REPLACE "\"message\": \"Hello\"" "\"message\": \"Nonsense\"" broken "${json}")
file(WRITE "${TEMP}/broken.json" "${broken}")
run(1 "broken.json" --net-encode "${TEMP}/broken.json" --output "${TEMP}/broken.swnetcap")
file(WRITE "${TEMP}/not-a-capture.swnetcap" "nope")
run(1 "not-a-capture.swnetcap" --net-dump "${TEMP}/not-a-capture.swnetcap")

# --assign-network-keys: a missing key and a repeated one get new keys, the first "door"
# stays; with a seed the result is the same every time. The source scene never changes.
set(keyed "${TEMP}/Arena/Scenes/keys.swscene")
file(WRITE "${keyed}" "shockwave_scene 2
editor_camera 0 7 18 0 -0.3

entity \"Door\"
  Transform
  NetworkObject
    key \"door\"
    profile \"engine.pose.v1\"
end

entity \"Door copy\"
  Transform
  NetworkObject
    key \"door\"
    profile \"engine.pose.v1\"
end

entity \"Lamp\"
  Transform
  NetworkObject
    profile \"engine.pose.v1\"
end
")
file(READ "${keyed}" keyedBefore)
run(0 "entity 'Door copy': \"door\" -> [0-9a-f-]+.*entity 'Lamp': \\(no key\\) -> [0-9a-f-]+.*2 of 3 network objects got new keys"
    --assign-network-keys --scene "${keyed}" --output "${TEMP}/keys-a.swscene" --seed 7)
run(0 "2 of 3" --assign-network-keys --scene "${keyed}" --output "${TEMP}/keys-b.swscene" --seed 7)
file(READ "${TEMP}/keys-a.swscene" a)
file(READ "${TEMP}/keys-b.swscene" b)
file(READ "${keyed}" keyedAfter)
if(NOT a STREQUAL b OR NOT a MATCHES "key \"door\"" OR NOT a MATCHES "editor_camera 0 7 18 0 -0.3" OR
   NOT keyedAfter STREQUAL keyedBefore)
    message(FATAL_ERROR "--assign-network-keys --seed should repeat, keep \"door\" and the header, and leave the scene:\n${a}")
endif()
run(0 "0 of 3 network objects got new keys" --assign-network-keys --scene "${TEMP}/keys-a.swscene"
    --output "${TEMP}/keys-c.swscene")
run(2 "--output is the scene itself" --assign-network-keys --scene "${keyed}" --output "${keyed}")
file(REMOVE "${keyed}")

# --network-manifest: a candidate with the network scene the manifest doesn't list yet.
file(WRITE "${TEMP}/Arena/Scenes/arena-extra.swscene" "shockwave_scene 2

entity \"Lamp\"
  Transform
  NetworkObject
    key \"lamp\"
    profile \"engine.pose.v1\"
end
")
run(0 "candidate adds scene 3 \"Scenes/arena-extra.swscene\".*content digest [0-9a-f]+"
    --network-manifest --output "${TEMP}/candidate.swnetmanifest")
file(READ "${TEMP}/candidate.swnetmanifest" candidate)
file(READ "${manifest}" manifestAfter)
if(NOT candidate MATCHES "scene 3 \"Scenes/arena-extra.swscene\"" OR NOT manifestAfter STREQUAL goodManifest)
    message(FATAL_ERROR "--network-manifest wrote the wrong candidate, or touched the manifest:\n${candidate}")
endif()
run(2 "--output is the manifest itself" --network-manifest --output "${manifest}")
file(REMOVE "${TEMP}/Arena/Scenes/arena-extra.swscene")

run(0 "0 problems" --check)
file(REMOVE_RECURSE "${TEMP}")
