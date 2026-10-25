# Batch 9 gate: a Steam game's package on a copy of Arena (docs/Packaging.md, "Steam").
# AppID 480 is refused unless it's a development package; steam_appid.txt and packet
# captures in the project never ship; a Steam-enabled engine ships its runtime library,
# and one without the SDK makes a package that plays offline (--host says why). The
# package is then moved and run from elsewhere. Nothing here starts Steam.
#   -DPLAYER=player -DPROJECT_DIR=Games/Arena -DTEMP=dir -DSTEAM_RUNTIME=libsteam_api.so [-DSTEAMWORKS=ON]
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}")
file(COPY "${PROJECT_DIR}/" DESTINATION "${TEMP}/Arena" PATTERN "Saved" EXCLUDE PATTERN "Source" EXCLUDE
     PATTERN "Tests" EXCLUDE)
set(project "${TEMP}/Arena/Arena.swproject")
set(settings "${TEMP}/Arena/Config/Network.swnetwork")
file(READ "${settings}" text)
string(REPLACE "enabled 0" "enabled 1" text "${text}")
string(REPLACE "steam_app_id 0" "steam_app_id 480" text "${text}")
file(WRITE "${settings}" "${text}")
# Developer files a project folder can have: none of them ship.
file(WRITE "${TEMP}/Arena/steam_appid.txt" "480\n")
file(WRITE "${TEMP}/Arena/Content/leftover.swnetcap" "not a capture")
file(MAKE_DIRECTORY "${TEMP}/Arena/Saved/Captures")
file(WRITE "${TEMP}/Arena/Saved/Captures/match.swnetcap" "not a capture")

function(package expected pattern)
    execute_process(COMMAND "${PLAYER}" --package --project "${project}" --no-build --no-archive
                            --output "${TEMP}/Packages" ${ARGN}
                    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL ${expected} OR NOT "${out}${err}" MATCHES "${pattern}")
        message(FATAL_ERROR "player --package ${ARGN}: expected exit ${expected} and '${pattern}', got ${result}:\n${out}\n${err}")
    endif()
    set(output "${out}" PARENT_SCOPE)
endfunction()

package(1 "steam_app_id is 480, Valve's shared test app")
if(EXISTS "${TEMP}/Packages")
    file(GLOB made "${TEMP}/Packages/*")
    if(made)
        message(FATAL_ERROR "A refused package left files behind: ${made}")
    endif()
endif()
package(0 "warning: Development package with AppID 480" --dev-package)
if(NOT output MATCHES "(^|\n)package ([^\n]+)")
    message(FATAL_ERROR "No package folder in:\n${output}")
endif()
set(package "${CMAKE_MATCH_2}")
if(STEAMWORKS)
    if(NOT EXISTS "${package}/Binaries/${STEAM_RUNTIME}" OR NOT output MATCHES "Steam runtime: ${STEAM_RUNTIME}")
        message(FATAL_ERROR "A Steam-enabled package should ship ${STEAM_RUNTIME}:\n${output}")
    endif()
else()
    if(NOT output MATCHES "warning: This engine was built without the Steamworks SDK, so the package plays offline only")
        message(FATAL_ERROR "An SDK-off package should say it plays offline only:\n${output}")
    endif()
    if(EXISTS "${package}/Binaries/${STEAM_RUNTIME}")
        message(FATAL_ERROR "An SDK-off package has no Steam runtime to ship")
    endif()
endif()
file(GLOB_RECURSE leaked "${package}/*steam_appid.txt" "${package}/*.swnetcap")
if(leaked)
    message(FATAL_ERROR "The package ships developer files: ${leaked}")
endif()
file(READ "${package}/README.txt" readme)
if(NOT readme MATCHES "Steam" AND NOT readme MATCHES "offline only")
    message(FATAL_ERROR "The README should say how online play works:\n${readme}")
endif()

# Moved somewhere else (a folder with a space), and started from a third place: it still
# finds its own project and code, and passes its own checks.
file(RENAME "${package}" "${TEMP}/moved here")
set(moved "${TEMP}/moved here")
if(NOT output MATCHES "\nrun ([^\n]+)")
    message(FATAL_ERROR "No launcher in:\n${output}")
endif()
get_filename_component(executable "${CMAKE_MATCH_1}" NAME)
set(game "${moved}/Binaries/${executable}")
file(MAKE_DIRECTORY "${TEMP}/elsewhere")
execute_process(COMMAND "${game}" --check WORKING_DIRECTORY "${TEMP}/elsewhere"
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0 OR NOT out MATCHES "0 problems")
    message(FATAL_ERROR "The moved package's --check failed (${result}):\n${out}\n${err}")
endif()
if(NOT STEAMWORKS)
    # Online play in an SDK-off package says why it can't, and nothing starts.
    execute_process(COMMAND "${game}" --host --renderer null WORKING_DIRECTORY "${TEMP}/elsewhere"
                    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 1 OR NOT "${out}${err}" MATCHES "this build has no Steamworks SDK")
        message(FATAL_ERROR "The SDK-off package's --host should fail with the reason (${result}):\n${out}\n${err}")
    endif()
endif()
file(REMOVE_RECURSE "${TEMP}")
