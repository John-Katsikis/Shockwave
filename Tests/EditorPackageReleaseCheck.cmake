# The editor's Package window in Release mode (S09, docs/Cooking.md#in-the-editor): on a copy
# of Reactor Run, `editor --package-release` ticks Release and presses Package, the same
# packageProject() as `player --package --release`. The package holds the cook and only
# what it lists.
#   -DEDITOR=editor -DPROJECT=Games/ShippingDemo -DTEMP=dir
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}")
file(COPY "${PROJECT}/" DESTINATION "${TEMP}/Game" PATTERN "Saved" EXCLUDE)
# Into the test's own folder, from the engine as it is.
file(APPEND "${TEMP}/Game/Config/Package.swpackage" "output \"${TEMP}/out\"\nbuild false\narchive false\n")
execute_process(COMMAND "${EDITOR}" --project "${TEMP}/Game" --prefs "${TEMP}/prefs" --package-release
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0 OR NOT "${out}${err}" MATCHES "package window: release ok, [0-9]+ files from the cook")
    message(FATAL_ERROR "editor --package-release: exit ${result}\n${out}\n${err}")
endif()
file(GLOB package "${TEMP}/out/ReactorRun-1.0.0-*")
list(FILTER package EXCLUDE REGEX "-symbols$")   # the kept symbols, beside the package
if(NOT EXISTS "${package}/Game/Cook.swcook" OR EXISTS "${package}/Game/Docs")
    message(FATAL_ERROR "the window's release package isn't made from the cook: ${package}")
endif()
message(STATUS "editor package release: passed")
