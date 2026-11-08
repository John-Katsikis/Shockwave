# player --package --release (S09, docs/Cooking.md#release-packages) on a copy of Reactor Run.
#   -DPLAYER=player -DPROJECT=Games/ShippingDemo -DTEMP=dir -DLIBRARY_SUFFIX=.so
#
# From a clean copy (no Saved/, so no cache) in a folder with spaces and non-ASCII letters,
# packaged from another working directory: the package holds only what its cook lists,
# plus the cook; its marker names the cook's build ID; the packaged player verifies it.
# Moved and made read-only, it plays menu -> training -> factory, saves, quits and resumes
# from yet another working directory. A missing module or library says so; a failed
# release cook keeps the last good package; a release is never a development package.
cmake_policy(SET CMP0057 NEW)
foreach(var PLAYER PROJECT TEMP LIBRARY_SUFFIX)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "PackageReleaseCheck.cmake needs -D${var}=...")
    endif()
endforeach()
if(EXISTS "${TEMP}")
    execute_process(COMMAND chmod -R u+w "${TEMP}")   # a read-only package from a past run
endif()
file(REMOVE_RECURSE "${TEMP}")
set(G "${TEMP}/Spaced Ünïcode folder/Reactor Run")
file(COPY "${PROJECT}/" DESTINATION "${G}" PATTERN "Saved" EXCLUDE)
file(MAKE_DIRECTORY "${TEMP}/elsewhere" "${TEMP}/out")
set(FLOWS "${PROJECT}/Tests/flow")
set(headless SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy)

function(run expect dir)
    execute_process(COMMAND ${CMAKE_COMMAND} -E env ${headless} ${ARGN} WORKING_DIRECTORY "${dir}"
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
function(package_release expect)
    run(${expect} "${TEMP}/elsewhere" "${PLAYER}" --project "${G}" --package --release --no-build --no-archive
        --output "${TEMP}/out")
    set(out_text "${out_text}" PARENT_SCOPE)
endfunction()

# ---- A release package, from a clean copy ----
package_release(0)
expect_text("From the cook: [0-9]+ files the game needs; [0-9]+ other project files left out" "the release")
expect_text("ok every game file matches the cook" "the package's check against its cook")
file(GLOB P "${TEMP}/out/ReactorRun-1.0.0-*")
string(REGEX REPLACE ".*ReactorRun-1\\.0\\.0-" "" platform "${P}")
if(NOT IS_DIRECTORY "${P}" OR NOT EXISTS "${P}/Game/Cook.swcook")
    message(FATAL_ERROR "no release package with its cook in ${TEMP}/out")
endif()
foreach(left_out Docs README.md CMakeLists.txt Tools Source Tests Content/PROVENANCE.md Config/Package.swpackage Saved/Cook)
    if(EXISTS "${P}/Game/${left_out}")
        message(FATAL_ERROR "the release package has Game/${left_out}, which the game never reads")
    endif()
endforeach()
foreach(kept ShippingDemo.swproject Config/Levels.swlevels Scenes/factory.swscene Content/UI/hud.rml
             Content/Audio/shot.wav Content/Shell/credits.txt Content/Localization/fr.swstrings)
    if(NOT EXISTS "${P}/Game/${kept}")
        message(FATAL_ERROR "the release package is missing Game/${kept}")
    endif()
endforeach()
# The folder says which build it is, and that is the cook's.
file(STRINGS "${P}/Binaries/shockwave-package.txt" marker_id REGEX "^build_id ")
file(STRINGS "${P}/Game/Cook.swcook" cook_id REGEX "^build_id ")
file(STRINGS "${P}/Binaries/shockwave-package.txt" marker_release REGEX "^release true")
if(NOT marker_id OR NOT marker_id STREQUAL cook_id OR NOT marker_release)
    message(FATAL_ERROR "the package's marker (${marker_id}, ${marker_release}) doesn't name its cook (${cook_id})")
endif()
run(0 "${TEMP}/elsewhere" "${P}/Binaries/ReactorRun" --cook-info "${P}/Game/Cook.swcook" --verify "${P}/Game")
expect_text("ok every file matches the cook" "the packaged player's --verify")

# ---- Moved and read-only: A -> B -> save -> quit -> resume, from another folder ----
set(M "${TEMP}/moved/Reactor Run")
file(MAKE_DIRECTORY "${TEMP}/moved")
file(RENAME "${P}" "${M}")
execute_process(COMMAND chmod -R a-w "${M}")
file(MAKE_DIRECTORY "${TEMP}/cwd")
foreach(flow full-run resume-quit resume-continue)
    if(flow STREQUAL "full-run")
        set(user "${TEMP}/user-full")
    else()
        set(user "${TEMP}/user-resume")
    endif()
    run(0 "${TEMP}/cwd" "${M}/ReactorRun" --renderer null --user-dir "${user}" --flow-test "${FLOWS}/${flow}.json"
        --json "${TEMP}/${flow}.json")
endforeach()
execute_process(COMMAND chmod -R u+w "${M}")

# ---- What breaks says so ----
# The game module gone: players are told to install again, not to build.
set(B "${TEMP}/broken")
file(COPY "${M}/" DESTINATION "${B}")
file(GLOB modules "${B}/Binaries/modules/*${LIBRARY_SUFFIX}")
file(REMOVE ${modules})
run(1 "${TEMP}/cwd" "${B}/ReactorRun" --check)
expect_text("game module \"shippingdemo\" couldn't load: .*install it again" "a missing game module")
if(out_text MATCHES "cmake --build")
    message(FATAL_ERROR "a packaged game told players to build it:\n${out_text}")
endif()
# An engine library gone: the system's loader names it.
file(REMOVE_RECURSE "${B}")
file(COPY "${M}/" DESTINATION "${B}")
file(REMOVE "${B}/Binaries/libShockwaveNavigation${LIBRARY_SUFFIX}")
execute_process(COMMAND ${CMAKE_COMMAND} -E env ${headless} "${B}/ReactorRun" --check
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(code EQUAL 0 OR NOT "${out}${err}" MATCHES "libShockwaveNavigation")
    message(FATAL_ERROR "a missing engine library: exit ${code}:\n${out}\n${err}")
endif()

# A failed release cook: the package fails and the last good one stays as it was.
file(RENAME "${M}" "${P}")
file(READ "${P}/Game/Cook.swcook" good)
# An absolute path in shipped text passes the project's checks; only the release policy
# refuses it.
file(READ "${G}/Content/Localization/en.swstrings" catalog)
file(APPEND "${G}/Content/Localization/en.swstrings" "hud.dev_note \"see /home/dev/notes.txt\"\n")
package_release(1)
expect_text("The release cook failed" "an absolute path")
expect_text("en.swstrings: names /home/dev/notes.txt, an absolute path" "an absolute path")
file(READ "${P}/Game/Cook.swcook" still)
if(NOT still STREQUAL good OR EXISTS "${TEMP}/out/ReactorRun-1.0.0-${platform}.partial")
    message(FATAL_ERROR "a failed release replaced the last good package, or left a half-made one")
endif()
file(WRITE "${G}/Content/Localization/en.swstrings" "${catalog}")
# A missing texture fails before the cook, at the project's checks; the old package stays.
file(RENAME "${G}/Content/UI/factory-badge.png" "${TEMP}/factory-badge.png")
package_release(1)
expect_text("factory-badge.png" "a missing texture")
file(READ "${P}/Game/Cook.swcook" still)
if(NOT still STREQUAL good)
    message(FATAL_ERROR "a failed release replaced the last good package")
endif()
file(RENAME "${TEMP}/factory-badge.png" "${G}/Content/UI/factory-badge.png")

# A release is never a test build.
run(2 "${TEMP}/elsewhere" "${PLAYER}" --project "${G}" --package --release --dev-package)
expect_text("--release and --dev-package don't go together" "--release --dev-package")
message(STATUS "package release: passed")
