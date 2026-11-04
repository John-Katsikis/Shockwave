# Editor Play on runtime levels (S03, docs/LevelLifetime.md): on a copy of
# Tests/fixtures/levels, `editor --level-cycles 100` presses Play, travels to the next
# level and presses Stop a hundred times, then checks the authored scene, undo history,
# selection and editor camera are exactly as they were.
#   -DEDITOR=editor -DFIXTURE=Tests/fixtures/levels -DTEMP=dir
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}")
file(COPY "${FIXTURE}/" DESTINATION "${TEMP}/Levels")
execute_process(COMMAND "${EDITOR}" --project "${TEMP}/Levels/Levels.swproject" --prefs "${TEMP}/prefs"
                        --level-cycles 100
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0 OR NOT out MATCHES "level cycles: 100 Play/travel/Stop cycles, the authored scene, undo, selection and camera are unchanged")
    message(FATAL_ERROR "editor --level-cycles: exit ${result}\n${out}\n${err}")
endif()
string(REGEX MATCHALL "Level \"a\" \\(epoch" arrived "${out}${err}")
list(LENGTH arrived count)
if(NOT count EQUAL 100)
    message(FATAL_ERROR "editor --level-cycles: travelled into level a ${count} times, not 100")
endif()
message(STATUS "editor level cycles: passed")
