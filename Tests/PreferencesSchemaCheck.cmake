# docs/PreferencesSchema.md is what player --preferences-schema writes (S08).
# cmake -DPLAYER=... -DSCHEMA=docs/PreferencesSchema.md -P PreferencesSchemaCheck.cmake
execute_process(COMMAND "${PLAYER}" --preferences-schema - RESULT_VARIABLE result OUTPUT_VARIABLE schema
    ERROR_VARIABLE errors)
file(READ "${SCHEMA}" checkedIn)
if(NOT result EQUAL 0 OR NOT schema STREQUAL checkedIn)
    message(FATAL_ERROR
        "docs/PreferencesSchema.md is out of date: run player --preferences-schema docs/PreferencesSchema.md\n${errors}")
endif()
