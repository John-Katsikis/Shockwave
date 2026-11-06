# The level catalog's command-line paths (S02, docs/Levels.md) on a copy of
# Tests/fixtures/levels:
#   - the good fixture checks clean, and --levels lists it as text and as JSON;
#   - each mistake, made one at a time, fails --check and --levels with its code;
#   - an unreachable level is a warning, not a failure;
#   - --level starts in a level, refuses unknown IDs and clashes with --scene;
#   - a circuit's Level.RequestTravel naming an unknown level or spawn fails --check (S05);
#   - --level-schema matches the checked-in docs/LevelSchema.md;
#   - a project without a catalog (Sample) has the implicit "startup" level.
#   -DPLAYER=player -DFIXTURE=Tests/fixtures/levels -DSAMPLE=Games/Sample/Sample.swproject
#   -DSCHEMA=docs/LevelSchema.md -DTEMP=dir
cmake_policy(SET CMP0054 NEW)
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}")
file(COPY "${FIXTURE}/" DESTINATION "${TEMP}/Levels")
set(root "${TEMP}/Levels")
set(project "${root}/Levels.swproject")
set(catalog "${root}/Config/Levels.swlevels")
file(READ "${catalog}" goodCatalog)
file(READ "${root}/Scenes/a.swscene" goodA)
file(READ "${root}/Scenes/b.swscene" goodB)

function(run expected pattern)
    execute_process(COMMAND "${PLAYER}" --project "${project}" ${ARGN}
                    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL ${expected} OR NOT "${out}${err}" MATCHES "${pattern}")
        message(FATAL_ERROR "player ${ARGN}: expected exit ${expected} and '${pattern}', got ${result}:\n${out}\n${err}")
    endif()
    set(out "${out}${err}" PARENT_SCOPE)
endfunction()

# Puts the good files back, then makes one mistake: --check fails, and --levels --json
# fails with the diagnostic code.
function(broken file from to code)
    file(WRITE "${catalog}" "${goodCatalog}")
    file(WRITE "${root}/Scenes/a.swscene" "${goodA}")
    file(WRITE "${root}/Scenes/b.swscene" "${goodB}")
    file(READ "${root}/${file}" text)
    string(FIND "${text}" "${from}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "LevelCheck: '${from}' isn't in ${file}")
    endif()
    string(REPLACE "${from}" "${to}" text "${text}")
    file(WRITE "${root}/${file}" "${text}")
    run(1 "problem" --check)
    run(1 "\"code\": \"(${code})\"" --levels --json -)
endfunction()

# ---- The good fixture ----
run(0 "ok Config/Levels.swlevels \\(3 levels\\).*0 problems" --check)
run(0 "levels Config/Levels.swlevels: 3 levels, entry menu" --levels)
foreach(line "level menu menu Scenes/menu.swscene, next a, 0 spawns, exits: none"
             "level a gameplay Scenes/a.swscene, next b, 2 spawns, exits: b:entrance"
             "level b gameplay Scenes/b.swscene, next menu, 1 spawn, exits: menu")
    string(FIND "${out}" "${line}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "--levels doesn't list '${line}':\n${out}")
    endif()
endforeach()
if(out MATCHES "unreachable")
    message(FATAL_ERROR "--levels calls a level of the good fixture unreachable:\n${out}")
endif()
run(0 "Wrote" --levels --json "${TEMP}/levels.json")
file(READ "${TEMP}/levels.json" json)
string(JSON format GET "${json}" format)
string(JSON mode GET "${json}" mode)
string(JSON implicit GET "${json}" implicit)
string(JSON entry GET "${json}" entry)
string(JSON count LENGTH "${json}" levels)
string(JSON ok GET "${json}" ok)
string(JSON aSpawns LENGTH "${json}" levels 1 spawns)
string(JSON aCheckpoint GET "${json}" levels 1 checkpoints 0)
string(JSON aExit GET "${json}" levels 1 exits 0 destination)
string(JSON aPreload GET "${json}" levels 1 preload 0)
string(JSON menuUi GET "${json}" levels 0 user_interface)
if(NOT format EQUAL 1 OR NOT mode STREQUAL "levels" OR NOT implicit STREQUAL "OFF" OR NOT entry STREQUAL "menu"
   OR NOT count EQUAL 3 OR NOT ok STREQUAL "ON" OR NOT aSpawns EQUAL 2 OR NOT aCheckpoint STREQUAL "halfway"
   OR NOT aExit STREQUAL "b" OR NOT aPreload STREQUAL "Content/UI/loading.rml" OR NOT menuUi STREQUAL "ON")
    message(FATAL_ERROR "--levels --json: ${json}")
endif()

# ---- One mistake at a time ----
broken("Scenes/a.swscene" "destination b" "destination nowhere" "LevelExitDestination")
broken("Scenes/a.swscene" "spawn entrance" "spawn back_door" "LevelExitSpawn")
broken("Scenes/b.swscene" "id entrance\n" "id entrance\n  Checkpoint\n    id entrance\n    spawn nowhere\n" "LevelCheckpointSpawn")
broken("Scenes/a.swscene" "id halfway\n  Checkpoint" "id start\n  Checkpoint" "LevelDuplicateSpawn")
broken("Scenes/b.swscene" "  Camera\n    primary true\n" "" "LevelNoCamera")
broken("Scenes/b.swscene" "shockwave_scene 2" "shockwave_scene 99" "LevelSceneBroken")
broken("Scenes/a.swscene" "id halfway\n  Checkpoint"
       "id halfway\n  PersistentIdentity\n    id 3f2c9a1e-6b7d-4c58-9e0a-1b2c3d4e5f60\n  Checkpoint"
       "LevelDuplicatePersistentId")
broken("Scenes/a.swscene" "id 3f2c9a1e-6b7d-4c58-9e0a-1b2c3d4e5f60" "id not-a-uuid" "LevelPersistentId")
broken("Config/Levels.swlevels" "default_spawn \"entrance\"" "default_spawn \"nowhere\"" "LevelSpawnMissing")
broken("Config/Levels.swlevels" "next \"menu\"" "next \"credits\"" "LevelUnknownNext")
broken("Config/Levels.swlevels" "entry \"menu\"" "entry \"title\"" "LevelUnknownEntry")
broken("Config/Levels.swlevels" "scene \"Scenes/b.swscene\"" "scene \"../outside.swscene\"" "LevelPathOutside")
broken("Config/Levels.swlevels" "scene \"Scenes/b.swscene\"" "scene \"Scenes/B.swscene\"" "LevelPathCase|LevelSceneMissing")
broken("Config/Levels.swlevels" "scene \"Scenes/b.swscene\"" "scene \"Scenes/gone.swscene\"" "LevelSceneMissing")
broken("Config/Levels.swlevels" "preload \"Content/UI/loading.rml\"" "preload \"Content/UI/gone.rml\""
       "LevelPreloadMissing")
broken("Config/Levels.swlevels" "level \"b\"" "level \"a\"" "LevelDuplicateId")
broken("Config/Levels.swlevels" "kind gameplay\n  default_spawn \"entrance\"" "kind boss\n  default_spawn \"entrance\""
       "LevelInvalidValue")
broken("Config/Levels.swlevels" "shockwave_levels 1" "shockwave_levels 9" "LevelUnsupportedVersion")

# ---- An unreachable level warns and still passes ----
file(WRITE "${root}/Scenes/a.swscene" "${goodA}")
file(WRITE "${root}/Scenes/b.swscene" "${goodB}")
file(WRITE "${catalog}" "${goodCatalog}level \"secret\"\n  scene \"Scenes/b.swscene\"\n  kind gameplay\nend\n")
run(0 "warning .*secret" --check)
run(0 "\"code\": \"LevelUnreachable\", \"severity\": \"warning\"" --levels --json -)
run(0 "level secret .*\\(unreachable\\).*ok" --levels)
file(WRITE "${catalog}" "${goodCatalog}")

# ---- Circuits naming levels (S05): a literal level and spawn must exist ----
function(travel level spawn)
    file(WRITE "${root}/Content/Circuits/travel.swcircuit"
         "shockwave_circuit 1\nnode go Event.BeginPlay\nnode travel Level.RequestTravel\n  exec go.then\n"
         "  in level = \"${level}\"\n  in spawn = \"${spawn}\"\n")
endfunction()
travel("b" "entrance")
run(0 "ok Content/Circuits/travel.swcircuit" --check)
travel("nowhere" "")
run(1 "travel.swcircuit:5: error UnknownLevel: node travel: Level.RequestTravel level \"nowhere\"" --check)
travel("b" "attic")
run(1 "error UnknownSpawn: node travel: Level.RequestTravel spawn \"attic\" isn't a LevelSpawn in Scenes/b.swscene"
    --check)
file(REMOVE "${root}/Content/Circuits/travel.swcircuit")

# ---- --level ----
run(0 "" --level b --renderer null --frames 3 --user-dir "${TEMP}/User")
run(1 "no such level \\(there are: menu, a, b\\)" --level zz --renderer null --frames 3 --user-dir "${TEMP}/User")
run(2 "LevelOptionConflict" --level b --scene Scenes/a.swscene)

# ---- The schema ----
execute_process(COMMAND "${PLAYER}" --level-schema - RESULT_VARIABLE result OUTPUT_VARIABLE schema)
file(READ "${SCHEMA}" checkedIn)
if(NOT result EQUAL 0 OR NOT schema STREQUAL checkedIn)
    message(FATAL_ERROR "docs/LevelSchema.md is out of date: run player --level-schema docs/LevelSchema.md")
endif()

# ---- No catalog: the startup scene is the only level ----
set(project "${SAMPLE}")
run(0 "levels none: no level catalog.*level startup gameplay Scenes/main.swscene.*ok" --levels)
message(STATUS "level catalog command line: all checks passed")
