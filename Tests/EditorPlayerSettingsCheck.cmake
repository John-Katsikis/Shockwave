# The editor's Player Settings previews (S08, docs/Preferences.md#in-the-editor): on a copy
# of Reactor Run, `editor --preview-language fr --preview-ui-scale 1.5` loads the game's
# catalogs, so the preview reads in French, and writes nothing into the project.
#   -DEDITOR=editor -DPROJECT=Games/ShippingDemo -DTEMP=dir
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}")
file(COPY "${PROJECT}/" DESTINATION "${TEMP}/Game" PATTERN "Saved" EXCLUDE)
execute_process(COMMAND "${EDITOR}" --project "${TEMP}/Game" --prefs "${TEMP}/prefs" --player-settings
                        --preview-language fr --preview-ui-scale 1.5 --frames 10
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0 OR NOT "${out}${err}" MATCHES "Previewing the game's UI in \"fr\" at 1.50x: hud.find_exit reads \"Trouvez la sortie\"")
    message(FATAL_ERROR "editor --preview-language fr: exit ${result}\n${out}\n${err}")
endif()
if(EXISTS "${TEMP}/Game/Config/Preferences.swprefs")
    message(FATAL_ERROR "a preview wrote the project's shipped preferences")
endif()
message(STATUS "editor player settings preview: passed")
