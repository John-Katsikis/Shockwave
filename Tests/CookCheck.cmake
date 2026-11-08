# player --cook (S09, docs/Cooking.md) on copies of Reactor Run.
#   -DPLAYER=player -DPROJECT=Games/ShippingDemo -DTEMP=dir -DPART=reproducibility|dependencies
#
# reproducibility: two cooks of one copy, and a cook of a copy in a folder with spaces and
# non-ASCII letters started from another working directory, write the same manifest byte
# for byte; --cook-info --verify passes on it.
# dependencies: a change to a stylesheet three levels share changes those three levels'
# closures and the content ID, not the menu's; a missing texture, an excluded document, a
# code-played sound without its include pattern, a file changed after the cook, an edited
# manifest, an absolute path in release, and a target this engine isn't built for each fail
# or show as they should.
cmake_policy(SET CMP0057 NEW)
foreach(var PLAYER PROJECT TEMP PART)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "CookCheck.cmake needs -D${var}=...")
    endif()
endforeach()
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}")

function(copy_game to)
    file(COPY "${PROJECT}/" DESTINATION "${to}" PATTERN "Saved" EXCLUDE)
endfunction()

# Runs the player; expect is 0 or 1. Leaves the output in out_text.
function(run expect dir)
    execute_process(COMMAND "${PLAYER}" ${ARGN} WORKING_DIRECTORY "${dir}"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
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

function(manifest_line file pattern var)
    file(STRINGS "${file}" lines REGEX "${pattern}")
    set(${var} "${lines}" PARENT_SCOPE)
endfunction()

if(PART STREQUAL "reproducibility")
    copy_game("${TEMP}/A")
    run(0 "${TEMP}" --project "${TEMP}/A" --cook --release --output "${TEMP}/one.swcook")
    run(0 "${TEMP}/A" --project "${TEMP}/A" --cook --release --output "${TEMP}/two.swcook")
    copy_game("${TEMP}/Spaced Ünïcode folder/Game")
    file(MAKE_DIRECTORY "${TEMP}/elsewhere")
    run(0 "${TEMP}/elsewhere" --project "${TEMP}/Spaced Ünïcode folder/Game" --cook --release
        --output "${TEMP}/three.swcook")
    file(READ "${TEMP}/one.swcook" one)
    file(READ "${TEMP}/two.swcook" two)
    file(READ "${TEMP}/three.swcook" three)
    if(NOT one STREQUAL two OR NOT one STREQUAL three)
        message(FATAL_ERROR "the same content cooked to different manifests:\n${one}\n---\n${two}\n---\n${three}")
    endif()
    if(one MATCHES "${TEMP}" OR one MATCHES "Spaced")
        message(FATAL_ERROR "the manifest names where it was made:\n${one}")
    endif()
    run(0 "${TEMP}" --cook-info "${TEMP}/three.swcook" --verify "${TEMP}/A")
    expect_text("ok every file matches the cook" "--cook-info --verify")
    expect_text("level factory: Scenes/factory.swscene" "--cook-info")
    run(0 "${TEMP}" --cook-info "${TEMP}/one.swcook" --json "${TEMP}/one.json")
    file(READ "${TEMP}/one.json" json)
    string(JSON entry GET "${json}" entry_level)
    string(JSON levels LENGTH "${json}" levels)
    if(NOT entry STREQUAL "menu" OR NOT levels EQUAL 4)
        message(FATAL_ERROR "--cook-info --json: entry ${entry}, ${levels} levels")
    endif()
    message(STATUS "cook reproducibility: passed")
    return()
endif()

# ---- dependencies ----
copy_game("${TEMP}/G")
set(G "${TEMP}/G")
run(0 "${TEMP}" --project "${G}" --cook)
# The manifest goes under the target this engine is built for, which the cook names.
string(REGEX MATCH "manifest ([^\n]*Saved/Cook/([a-z0-9_-]+)/Cook\\.swcook)" _ "${out_text}")
set(M "${CMAKE_MATCH_1}")
set(native "${CMAKE_MATCH_2}")
if(NOT EXISTS "${M}")
    message(FATAL_ERROR "the cook didn't say where its manifest is:\n${out_text}")
endif()
manifest_line("${M}" "^level " before)
manifest_line("${M}" "^content_id " before_id)
manifest_line("${M}" "^asset \"Content/Audio/shot.wav\"" shot)
if(NOT shot)
    message(FATAL_ERROR "the include pattern's sound isn't in the cook: ${shot}")
endif()

# A shared stylesheet changes: the three levels that use it change, the menu doesn't.
file(APPEND "${G}/Content/UI/demo.rcss" "\n/* changed */\n")
run(0 "${TEMP}" --project "${G}" --cook)
manifest_line("${M}" "^level " after)
manifest_line("${M}" "^content_id " after_id)
if(before_id STREQUAL after_id)
    message(FATAL_ERROR "the content ID didn't change with a stylesheet")
endif()
run(0 "${TEMP}" --project "${G}" --cook)   # the same again: nothing more changes
file(STRINGS "${M}" again REGEX "^level ")
if(NOT again STREQUAL after)
    message(FATAL_ERROR "a second cook of the same content differs")
endif()
set(changed "")
foreach(line IN LISTS after)
    if(NOT line IN_LIST before)
        string(REGEX MATCH "^level ([a-z]+) " _ "${line}")
        list(APPEND changed "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(NOT changed STREQUAL "ending;factory;training")
    message(FATAL_ERROR "the levels whose closures changed with the shared stylesheet: ${changed}, not ending;factory;training")
endif()

# A file changed after the cook: --verify says which.
file(APPEND "${G}/Content/UI/hud.rml" "\n<!-- edited after the cook -->\n")
run(1 "${TEMP}" --cook-info "${M}" --verify "${G}")
expect_text("Content/UI/hud.rml: changed since the cook" "--verify after an edit")
# An edited manifest: its IDs no longer match.
file(READ "${M}" manifest)
string(REPLACE "\"Content/UI/demo.rcss\" \"stylesheet\" " "\"Content/UI/demo.rcss\" \"stylesheet\" 1" edited "${manifest}")
file(WRITE "${TEMP}/edited.swcook" "${edited}")
run(1 "${TEMP}" --cook-info "${TEMP}/edited.swcook")
expect_text("its IDs don't match its records" "an edited manifest")

# A missing texture: the cook fails and names it; the last good manifest stays.
file(RENAME "${G}/Content/UI/factory-badge.png" "${TEMP}/factory-badge.png")
run(1 "${TEMP}" --project "${G}" --cook)
expect_text("factory-badge.png" "a missing texture")
expect_text("Cook failed" "a missing texture")
if(NOT EXISTS "${M}")
    message(FATAL_ERROR "a failed cook removed the last good manifest")
endif()
file(RENAME "${TEMP}/factory-badge.png" "${G}/Content/UI/factory-badge.png")

# An excluded document a level needs.
file(READ "${G}/Config/Package.swpackage" settings)
file(APPEND "${G}/Config/Package.swpackage" "exclude \"Content/UI/hud.rml\"\n")
run(1 "${TEMP}" --project "${G}" --cook)
expect_text("Content/UI/hud.rml: excluded by Config/Package.swpackage, but level factory and others needs it"
            "an excluded document")
file(WRITE "${G}/Config/Package.swpackage" "${settings}")

# Without the include pattern, the sounds game code plays aren't in the cook.
string(REPLACE "include \"Content/Audio/*.wav\"" "" without "${settings}")
file(WRITE "${G}/Config/Package.swpackage" "${without}")
run(0 "${TEMP}" --project "${G}" --cook)
file(STRINGS "${M}" shot REGEX "^asset \"Content/Audio/shot.wav\"")
if(shot)
    message(FATAL_ERROR "a sound nothing names is in the cook without an include pattern: ${shot}")
endif()
file(WRITE "${G}/Config/Package.swpackage" "${settings}")

# An absolute path in a release's text.
file(READ "${G}/Content/Localization/en.swstrings" catalog)
file(APPEND "${G}/Content/Localization/en.swstrings" "hud.dev_note \"see /home/dev/notes.txt\"\n")
run(0 "${TEMP}" --project "${G}" --cook)   # a development cook allows it
run(1 "${TEMP}" --project "${G}" --cook --release)
expect_text("Content/Localization/en.swstrings: names /home/dev/notes.txt, an absolute path" "an absolute path")
file(WRITE "${G}/Content/Localization/en.swstrings" "${catalog}")
file(COPY "${PROJECT}/Content/UI/hud.rml" DESTINATION "${G}/Content/UI")
run(0 "${TEMP}" --project "${G}" --check --release)
expect_text("ok release policy \\(4 levels" "--check --release")

# A target this engine isn't built for.
if(native STREQUAL "windows-x86_64")
    set(other "linux-x86_64")
else()
    set(other "windows-x86_64")
endif()
run(1 "${TEMP}" --project "${G}" --cook --target ${other})
expect_text("can't cook for ${other}: this engine is built for ${native}" "the wrong target")
message(STATUS "cook dependencies: passed")
