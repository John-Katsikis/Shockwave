# Per-user folders for an installed game (S01, docs/UserPaths.md), end to end on a packaged
# Sample in a folder with a space and a non-ASCII name:
#   - installed, it resolves under HOME by its application ID, and fails clearly without HOME;
#   - a save from another directory lands in the profile, and the package doesn't change;
#   - another profile sees none of it; the package moved still finds it;
#   - --user-dir inside the package is refused;
#   - development saves import by copy, never over a slot, and --preview changes nothing;
#   - renaming the game doesn't change its application ID.
#   -DPLAYER=player -DPROJECT=Games/Sample/Sample.swproject -DTEMP=dir
cmake_policy(SET CMP0054 NEW)
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}")
set(out "${TEMP}/Install dir é")
execute_process(COMMAND "${PLAYER}" --package --project "${PROJECT}" --no-build --no-archive --output "${out}"
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0 OR NOT output MATCHES "\npackage ([^\n]+)\n")
    message(FATAL_ERROR "Packaging failed (${result}):\n${output}\n${errors}")
endif()
set(package "${CMAKE_MATCH_1}")
set(home "${TEMP}/home ü")
set(root "${home}/.local/share/shockwave.sample")
file(MAKE_DIRECTORY "${TEMP}/elsewhere")

# Runs the packaged game from an unrelated directory with HOME pointing at the test's home.
function(game expected pattern)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "HOME=${home}" --unset=XDG_DATA_HOME
                        SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy "${package}/Binaries/ShockwaveSample" ${ARGN}
                    WORKING_DIRECTORY "${TEMP}/elsewhere" RESULT_VARIABLE result OUTPUT_VARIABLE o ERROR_VARIABLE e)
    if(NOT result EQUAL ${expected} OR NOT "${o}${e}" MATCHES "${pattern}")
        message(FATAL_ERROR "game ${ARGN}: expected exit ${expected} and '${pattern}', got ${result}:\n${o}\n${e}")
    endif()
    set(out "${o}" PARENT_SCOPE)
endfunction()

function(hashes dir var)
    file(GLOB_RECURSE files RELATIVE "${dir}" "${dir}/*")
    list(SORT files)
    set(all "")
    foreach(f ${files})
        file(SHA256 "${dir}/${f}" h)
        string(APPEND all "${f} ${h}\n")
    endforeach()
    set(${var} "${all}" PARENT_SCOPE)
endfunction()
hashes("${package}" before)

# ---- Where it writes ----
game(0 "" --user-paths --json -)
string(JSON source GET "${out}" source)
string(JSON id GET "${out}" application_id)
string(JSON installed GET "${out}" installed)
string(JSON saves GET "${out}" folders saves path)
string(JSON savesExist GET "${out}" folders saves exists)
if(NOT source STREQUAL "installed" OR NOT id STREQUAL "shockwave.sample" OR NOT installed STREQUAL "ON"
   OR NOT saves STREQUAL "${root}/profiles/default/Saves" OR savesExist)
    message(FATAL_ERROR "--user-paths in the package: ${out}")
endif()
if(EXISTS "${root}")
    message(FATAL_ERROR "--user-paths created ${root}; resolving should create nothing")
endif()
# No HOME: a clear failure, and nothing is written anywhere.
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=HOME --unset=XDG_DATA_HOME
                    "${package}/Binaries/ShockwaveSample" --user-paths
                WORKING_DIRECTORY "${TEMP}/elsewhere" RESULT_VARIABLE result OUTPUT_VARIABLE o ERROR_VARIABLE e)
if(NOT result EQUAL 1 OR NOT "${o}${e}" MATCHES "HOME isn't set")
    message(FATAL_ERROR "--user-paths without HOME should fail with the reason (${result}):\n${o}\n${e}")
endif()
file(GLOB stray "${TEMP}/elsewhere/*")
if(stray)
    message(FATAL_ERROR "The game wrote into its working directory: ${stray}")
endif()

# ---- A save, from somewhere else ----
game(0 "Wrote [^\n]*profiles/default/Saves/slot one.swsave" --renderer null --save "slot one" --frames 5)
if(NOT EXISTS "${root}/profiles/default/Saves/slot one.swsave")
    message(FATAL_ERROR "The save isn't in the default profile under ${root}")
endif()
game(0 "slot one " --saves)
hashes("${package}" after)
if(NOT before STREQUAL after)
    message(FATAL_ERROR "Playing changed the installed package")
endif()
file(GLOB stray "${TEMP}/elsewhere/*")
if(stray)
    message(FATAL_ERROR "The game wrote into its working directory: ${stray}")
endif()

# Another profile has its own saves.
game(0 "No save games" --saves --profile p2)
game(2 "profile" --saves --profile "Bad Profile")

# The package moved elsewhere still finds its player's saves (they're per user, not per folder).
file(RENAME "${package}" "${TEMP}/moved package")
set(package "${TEMP}/moved package")
game(0 "slot one " --saves)

# Never inside the game's own folder.
game(1 "inside the installed game" --user-paths --user-dir "${package}/Saved")
game(0 "\"source\": \"override\"" --user-paths --json - --user-dir "${TEMP}/override")

# ---- Importing saves made before (development, or an old version) ----
file(MAKE_DIRECTORY "${TEMP}/legacy")
file(COPY "${root}/profiles/default/Saves/slot one.swsave" DESTINATION "${TEMP}/legacy")
file(RENAME "${TEMP}/legacy/slot one.swsave" "${TEMP}/legacy/older.swsave")
file(COPY "${root}/profiles/default/Saves/slot one.swsave" DESTINATION "${TEMP}/legacy")
file(WRITE "${TEMP}/legacy/broken.swsave" "not a save")
file(SHA256 "${root}/profiles/default/Saves/slot one.swsave" kept)
game(1 "would copy 1, kept 1, failed 1" --import-legacy-saves "${TEMP}/legacy" --preview)
if(EXISTS "${root}/profiles/default/Saves/older.swsave")
    message(FATAL_ERROR "--preview copied a save")
endif()
game(1 "copy older.*keep slot one: the profile already has this slot.*copied 1, kept 1, failed 1"
     --import-legacy-saves "${TEMP}/legacy")
file(REMOVE "${TEMP}/legacy/broken.swsave")
game(0 "copied 0, kept 2, failed 0" --import-legacy-saves "${TEMP}/legacy")
file(SHA256 "${root}/profiles/default/Saves/slot one.swsave" still)
if(NOT EXISTS "${TEMP}/legacy/older.swsave" OR NOT kept STREQUAL still)
    message(FATAL_ERROR "Importing moved an original or wrote over a profile's save")
endif()
game(0 "older " --saves)
game(1 "isn't a folder" --import-legacy-saves "${TEMP}/missing")
game(2 "--preview" --preview)

# ---- Renaming the game keeps its ID (development, from a copy of the project) ----
get_filename_component(projectDir "${PROJECT}" DIRECTORY)
file(COPY "${projectDir}/" DESTINATION "${TEMP}/Project" PATTERN "Saved" EXCLUDE)
file(GLOB projectFile "${TEMP}/Project/*.swproject")
file(READ "${projectFile}" text)
string(REGEX REPLACE "name \"[^\"]*\"" "name \"A Renamed Game\"" text "${text}")
file(WRITE "${projectFile}" "${text}")
execute_process(COMMAND "${PLAYER}" --project "${projectFile}" --user-paths
                RESULT_VARIABLE result OUTPUT_VARIABLE o ERROR_VARIABLE e)
if(NOT result EQUAL 0 OR NOT o MATCHES "source development" OR NOT o MATCHES "application_id shockwave.sample"
   OR NOT o MATCHES "Project/Saved")
    message(FATAL_ERROR "A renamed game's --user-paths (${result}):\n${o}\n${e}")
endif()
file(REMOVE_RECURSE "${TEMP}")
