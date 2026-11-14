# player --verify-install (S13, docs/Release.md#platforms) on a release package of Reactor Run.
#   -DPLAYER=player -DPROJECT=Games/ShippingDemo -DTEMP=dir -DLIBRARY_SUFFIX=.so
#
# The package's marker names the program players start. Installed read-only at a path with
# spaces and non-ASCII letters and started with a clean environment from another folder, it
# passes its own --check, saves and quits, then continues in a second run; its data lands in
# the fresh home and the install folder stays unchanged. The record it keeps reads back,
# and the platform gate says what it proves (not the graphics, on the null renderer). The
# contract's acceptance scenario plays by default. A broken package fails its check and
# fails the gate. A folder that isn't a package, or a work folder the tool didn't make, is
# refused and left alone.
foreach(var PLAYER PROJECT TEMP LIBRARY_SUFFIX)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "InstalledGameCheck.cmake needs -D${var}=...")
    endif()
endforeach()
if(EXISTS "${TEMP}")
    execute_process(COMMAND chmod -R u+w "${TEMP}")
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
expect_text("skipped +update" "updates, for part 2")
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
if(NOT ok OR NOT steps EQUAL 7 OR NOT first STREQUAL "install" OR NOT hash_length EQUAL 64 OR
   NOT archive_length EQUAL 64)
    message(FATAL_ERROR "the verify-install report: ${json}")
endif()
# The archive's hash is the archive's.
file(SHA256 "${P}.tar.gz" archive_file)
if(NOT archive STREQUAL archive_file)
    message(FATAL_ERROR "archive_sha256 ${archive} isn't the archive's (${archive_file})")
endif()
set(RECORD "${G}/Release/Installs/test-machine-linux-x86-64.swinstall")
if(NOT EXISTS "${RECORD}")
    message(FATAL_ERROR "no install record at ${RECORD}")
endif()
file(STRINGS "${RECORD}" result REGEX "^result ")
if(NOT result STREQUAL "result passed")
    message(FATAL_ERROR "the record says ${result}")
endif()
# The gate reads it: the install is proven, the graphics aren't (the null renderer).
run(0 "${PLAYER}" --project "${G}" --release-info)
expect_text("gate platform:linux-x86_64 unverified \\(S13\\): Release/Installs/test-machine-linux-x86-64.swinstall doesn't qualify linux-x86_64: it played on the null renderer"
    "the platform gate")

# ---- The contract's acceptance scenario plays when none is given ----
run(0 "${PLAYER}" --verify-install "${P}" --project "${G}" --renderer null)
expect_text("passed +scenario resume-quit.json" "the acceptance scenario")

# ---- A broken package fails, and so does the gate ----
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)   # a newer record
set(B "${TEMP}/broken/ReactorRun-1.0.0-broken")
file(COPY "${P}/" DESTINATION "${B}")
file(GLOB modules "${B}/Binaries/modules/*${LIBRARY_SUFFIX}")
file(REMOVE ${modules})
run(1 "${PLAYER}" --verify-install "${B}" --flow-test "${FLOWS}/resume-quit.json" --renderer null
    --install-record "Broken machine" --project "${G}")
expect_text("failed +check .*--check: exit code 1" "the broken package's check")
expect_text("verify-install failed" "the broken package's verdict")
run(0 "${PLAYER}" --project "${G}" --release-info)
expect_text("gate platform:linux-x86_64 failed \\(S13\\): Release/Installs/broken-machine-linux-x86-64.swinstall: step \"check\" failed"
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
