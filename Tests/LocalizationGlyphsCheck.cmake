# player --check and fonts (S08, docs/Localization.md#checks): Reactor Run checks clean,
# then with a Japanese catalog, whose characters no font in the project or the engine draws,
# --check fails and names them.
#   -DPLAYER=player -DPROJECT=Games/ShippingDemo -DTEMP=dir
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}")
file(COPY "${PROJECT}/" DESTINATION "${TEMP}/Game" PATTERN "Saved" EXCLUDE)
execute_process(COMMAND "${PLAYER}" --project "${TEMP}/Game" --check
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0 OR "${out}${err}" MATCHES "no font draws")
    message(FATAL_ERROR "player --check on Reactor Run: exit ${result}\n${out}\n${err}")
endif()
file(WRITE "${TEMP}/Game/Content/Localization/ja.swstrings"
     "swstrings 1\nhud.find_exit \"出口を探す\"\nhud.health \"体力\"\n")
execute_process(COMMAND "${PLAYER}" --project "${TEMP}/Game" --check
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(result EQUAL 0 OR NOT "${out}${err}" MATCHES "ja.swstrings: 2 texts have 7 characters no font draws \\(U\\+51FA")
    message(FATAL_ERROR "player --check with a Japanese catalog and no font for it: exit ${result}\n${out}\n${err}")
endif()
message(STATUS "localization glyphs: passed")
