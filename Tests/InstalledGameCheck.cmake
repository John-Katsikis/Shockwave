# player --verify-install (S13, docs/Release.md#platforms) on a release package of Reactor Run.
#   -DPLAYER=player -DPROJECT=Games/ShippingDemo -DTEMP=dir -DLIBRARY_SUFFIX=.so
#
# The package's marker names the program players start. Installed read-only at a path with
# spaces and non-ASCII letters and started with a clean environment from another folder, it
# passes its own --check, saves and quits, then continues in a second run; its data lands in
# the fresh home and the install folder stays unchanged. The record it keeps reads back,
# and the platform gate says what it proves (not the graphics, on the null renderer). The
# contract's acceptance scenario plays by default. With --update-from, 1.0.0 is installed,
# played and saved first, then 1.1.0 (a newer save schema) replaces it and continues; 1.0.0
# put back refuses the newer saves and overwrites nothing; a reinstall elsewhere still
# finds them. After that only the renderer keeps the gate unverified. A broken package fails its check and
# fails the gate. A folder that isn't a package, or a work folder the tool didn't make, is
# refused and left alone.
foreach(var PLAYER PROJECT TEMP LIBRARY_SUFFIX)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "InstalledGameCheck.cmake needs -D${var}=...")
    endif()
endforeach()
if(EXISTS "${TEMP}" AND NOT CMAKE_HOST_WIN32)
    execute_process(COMMAND chmod -R u+w "${TEMP}")
endif()
# Windows packages have no archive yet (docs/Packaging.md#limits).
if(CMAKE_HOST_WIN32)
    set(ARCHIVES OFF)
else()
    set(ARCHIVES ON)
endif()
file(REMOVE_RECURSE "${TEMP}")
set(G "${TEMP}/Spaced Ünïcode/Reactor Run")
file(COPY "${PROJECT}/" DESTINATION "${G}" PATTERN "Saved" EXCLUDE)
file(MAKE_DIRECTORY "${TEMP}/elsewhere" "${TEMP}/out")
file(WRITE "${G}/Config/Release.swrelease" "shockwave_release 1
application_id \"com.shockwave.reactorrun.installtest\"
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
acceptance_scenario \"Tests/flow/resume-quit.json\"
")
set(FLOWS "${PROJECT}/Tests/flow")
set(headless SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy)

function(run expect)
    execute_process(COMMAND ${CMAKE_COMMAND} -E env ${headless} ${ARGN} WORKING_DIRECTORY "${TEMP}/elsewhere"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
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

# ---- A release package; its marker names the program players start ----
run(0 "${PLAYER}" --project "${G}" --package --release --no-build --output "${TEMP}/out")
file(GLOB P "${TEMP}/out/ReactorRun-1.0.0-*")
list(FILTER P EXCLUDE REGEX "(-symbols|\\.tar\\.gz)$")
if(NOT IS_DIRECTORY "${P}")
    message(FATAL_ERROR "no release package in ${TEMP}/out")
endif()
# This machine's platform, as packages and contracts name it; the contract names it too.
string(REGEX REPLACE ".*ReactorRun-1\\.0\\.0-" "" platform "${P}")
string(REPLACE "_" "-" platform_file "${platform}")
file(READ "${G}/Config/Release.swrelease" contract)
string(REPLACE "platform linux-x86_64" "platform ${platform}" contract "${contract}")
file(WRITE "${G}/Config/Release.swrelease" "${contract}")
file(STRINGS "${P}/Binaries/shockwave-package.txt" executable REGEX "^executable ")
if(NOT executable STREQUAL "executable \"ReactorRun\"")
    message(FATAL_ERROR "the package's marker doesn't name its program: ${executable}")
endif()

# ---- Installed and played: save and quit, then continue ----
set(WORK "${TEMP}/work")
run(0 "${PLAYER}" --verify-install "${P}" --flow-test "${FLOWS}/resume-quit.json"
    --flow-test "${FLOWS}/resume-continue.json" --renderer null --install-record "Test machine" --project "${G}"
    --install-dir "${WORK}" --json "${TEMP}/verify.json")
expect_text("passed +install .*read-only folder with spaces and non-ASCII letters" "the install step")
expect_text("passed +check " "the installed game's --check")
expect_text("passed +scenario resume-quit.json .*passed +scenario resume-continue.json .*null renderer" "both scenarios")
expect_text("passed +user data .*the install folder is unchanged" "the player's data")
expect_text("skipped +update .*needs --update-from" "no update without --update-from")
expect_text("verify-install passed" "the verdict")
if(EXISTS "${WORK}")
    message(FATAL_ERROR "--verify-install left its work folder behind: ${WORK}")
endif()
file(READ "${TEMP}/verify.json" json)
string(JSON ok GET "${json}" ok)
string(JSON steps LENGTH "${json}" steps)
string(JSON first GET "${json}" steps 0 name)
string(JSON hash GET "${json}" package_sha256)
string(JSON archive GET "${json}" archive_sha256)
string(LENGTH "${hash}" hash_length)
string(LENGTH "${archive}" archive_length)
if(ARCHIVES)
    set(archive_expected 64)
else()
    set(archive_expected 0)
endif()
if(NOT ok OR NOT steps EQUAL 8 OR NOT first STREQUAL "install" OR NOT hash_length EQUAL 64 OR
   NOT archive_length EQUAL archive_expected)
    message(FATAL_ERROR "the verify-install report: ${json}")
endif()
# The archive's hash is the archive's.
if(ARCHIVES)
    file(SHA256 "${P}.tar.gz" archive_file)
    if(NOT archive STREQUAL archive_file)
        message(FATAL_ERROR "archive_sha256 ${archive} isn't the archive's (${archive_file})")
    endif()
endif()
set(RECORD "${G}/Release/Installs/test-machine-${platform_file}.swinstall")
if(NOT EXISTS "${RECORD}")
    message(FATAL_ERROR "no install record at ${RECORD}")
endif()
file(STRINGS "${RECORD}" result REGEX "^result ")
if(NOT result STREQUAL "result passed")
    message(FATAL_ERROR "the record says ${result}")
endif()
# The gate reads it: the install is proven, the graphics aren't (the null renderer).
run(0 "${PLAYER}" --project "${G}" --release-info)
expect_text("gate platform:${platform} unverified \\(S13\\): Release/Installs/test-machine-${platform_file}.swinstall doesn't qualify ${platform}: it played on the null renderer"
    "the platform gate")

# ---- The contract's acceptance scenario plays when none is given ----
run(0 "${PLAYER}" --verify-install "${P}" --project "${G}" --renderer null)
expect_text("passed +scenario resume-quit.json" "the acceptance scenario")

# ---- An update: 1.0.0 played and saved, replaced by 1.1.0 with changed content and a newer
# save schema; it continues the old save and keeps the old file; 1.0.0 put back explains
# and overwrites nothing; removed and installed elsewhere, the saves are still found ----
set(G2 "${TEMP}/Spaced Ünïcode/Reactor Run 2")
file(COPY "${G}/" DESTINATION "${G2}" PATTERN "Release" EXCLUDE PATTERN "Saved" EXCLUDE)
foreach(edit "ShippingDemo.swproject|save_schema 1|save_schema 2"
             "Config/Package.swpackage|version \"1.0.0\"|version \"1.1.0\""
             "Config/Release.swrelease|release_version \"1.0.0\"|release_version \"1.1.0\"")
    string(REPLACE "|" ";" parts "${edit}")
    list(GET parts 0 file)
    list(GET parts 1 from)
    list(GET parts 2 to)
    file(READ "${G2}/${file}" text)
    string(FIND "${text}" "${from}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "the update's project: no '${from}' in ${file}")
    endif()
    string(REPLACE "${from}" "${to}" text "${text}")
    file(WRITE "${G2}/${file}" "${text}")
endforeach()
file(APPEND "${G2}/Content/Shell/credits.txt" "Thanks for playing version 1.1\n")
run(0 "${PLAYER}" --project "${G2}" --package --release --no-build --no-archive --output "${TEMP}/out2")
set(P2 "${TEMP}/out2/ReactorRun-1.1.0-${platform}")
if(NOT IS_DIRECTORY "${P2}")
    message(FATAL_ERROR "no 1.1.0 release package at ${P2}")
endif()
run(0 "${PLAYER}" --verify-install "${P2}" --update-from "${P}" --flow-test "${FLOWS}/resume-quit.json"
    --flow-test "${FLOWS}/resume-continue.json" --renderer null --install-record "Update machine" --project "${G2}")
expect_text("passed +install previous .*version 1\\.0\\.0" "the previous version's install")
expect_text("passed +scenario resume-quit.json \\(previous version\\)" "the previous version's play")
expect_text("passed +update .*from 1\\.0\\.0 .*save schema 1\\) to 1\\.1\\.0 .*save schema 2\\): [1-9][0-9]* saves? carried over.*kept as it was"
    "the update")
expect_text("passed +downgrade .*version 1\\.0\\.0 started over .*left every file as it was.*newer version"
    "the downgrade")
expect_text("passed +reinstall .*removed, installed again .*every one loads" "the reinstall")
expect_text("verify-install passed" "the update's verdict")
set(RECORD2 "${G2}/Release/Installs/update-machine-${platform_file}.swinstall")
file(STRINGS "${RECORD2}" result REGEX "^result |^step \"(update|downgrade|reinstall)\" ")
list(LENGTH result result_lines)
list(FILTER result EXCLUDE REGEX "passed")
if(NOT result_lines EQUAL 4 OR result)
    message(FATAL_ERROR "the update's record: ${result}")
endif()
file(STRINGS "${RECORD2}" previous REGEX "^previous_version ")
if(NOT previous STREQUAL "previous_version \"1.0.0\"")
    message(FATAL_ERROR "the update's record doesn't name the version it updated from: ${previous}")
endif()
# Every step passed; what the gate still wants is a real GPU.
run(0 "${PLAYER}" --project "${G2}" --release-info)
expect_text("gate platform:${platform} unverified \\(S13\\): Release/Installs/update-machine-${platform_file}.swinstall doesn't qualify ${platform}: it played on the null renderer"
    "the platform gate after an update")
# An update needs a save to carry: one scenario isn't enough.
run(2 "${PLAYER}" --verify-install "${P2}" --update-from "${P}" --flow-test "${FLOWS}/resume-quit.json")
expect_text("give at least two" "an update with one scenario")

# ---- A broken package fails, and so does the gate ----
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)   # a newer record
set(B "${TEMP}/broken/ReactorRun-1.0.0-broken")
file(COPY "${P}/" DESTINATION "${B}")
# Its game modules gone (a modular build) and a level's scene missing (every build).
file(GLOB modules "${B}/Binaries/modules/*${LIBRARY_SUFFIX}")
file(REMOVE ${modules} "${B}/Game/Scenes/ending.swscene")
run(1 "${PLAYER}" --verify-install "${B}" --flow-test "${FLOWS}/resume-quit.json" --renderer null
    --install-record "Broken machine" --project "${G}")
expect_text("failed +check .*--check: exit code 1" "the broken package's check")
expect_text("verify-install failed" "the broken package's verdict")
run(0 "${PLAYER}" --project "${G}" --release-info)
expect_text("gate platform:${platform} failed \\(S13\\): Release/Installs/broken-machine-${platform_file}.swinstall: step \"check\" failed"
    "the gate after a failed install")

# ---- What it refuses ----
run(1 "${PLAYER}" --verify-install "${G}" --flow-test "${FLOWS}/resume-quit.json")
expect_text("not a Shockwave package" "a project folder isn't a package")
file(MAKE_DIRECTORY "${TEMP}/someone else's")
file(WRITE "${TEMP}/someone else's/notes.txt" "keep me")
run(1 "${PLAYER}" --verify-install "${P}" --flow-test "${FLOWS}/resume-quit.json" --install-dir "${TEMP}/someone else's")
expect_text("wasn't made by --verify-install" "a foreign work folder")
if(NOT EXISTS "${TEMP}/someone else's/notes.txt")
    message(FATAL_ERROR "--verify-install removed a folder it didn't make")
endif()
run(2 "${PLAYER}" --verify-install "${P}" --flow-test "${FLOWS}/resume-quit.json" --install-record "x")
expect_text("--install-record keeps the record in a project's Release/Installs" "a record with nowhere to go")
run(2 "${PLAYER}" --install-record "x")
expect_text("go with --verify-install" "--install-record alone")
message(STATUS "installed game flow: passed")
