# Level authoring and failures by kind (S07, docs/Levels.md#checks). On a copy of Reactor
# Run, `editor --level-authoring` uses the Levels window's own actions: New level, a
# spawn, checkpoint and exit placed in it from the Create menu's code, Duplicate level,
# Save, then Validate, which must find no errors. On a second copy, broken three ways (an
# objective's target that isn't there, a circuit file that isn't there, an exit to a level
# that isn't there), Validate and `player --check` must each name every problem under its
# own kind: content, circuit and gameplay, never a generic "the level won't run".
# cmake -DEDITOR=... -DPLAYER=... -DGAME=Games/ShippingDemo -DTEMP=... -P DemoAuthoringCheck.cmake
foreach(var EDITOR PLAYER GAME TEMP)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "DemoAuthoringCheck.cmake needs -D${var}=...")
    endif()
endforeach()
file(REMOVE_RECURSE "${TEMP}")
foreach(copy good broken)
    file(MAKE_DIRECTORY "${TEMP}/${copy}")
    foreach(part Config Content Scenes ShippingDemo.swproject)
        file(COPY "${GAME}/${part}" DESTINATION "${TEMP}/${copy}")
    endforeach()
endforeach()

function(author copy expect_code)
    execute_process(COMMAND "${EDITOR}" --project "${TEMP}/${copy}/ShippingDemo.swproject"
            --prefs "${TEMP}/prefs-${copy}" --level-authoring
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL expect_code OR NOT out MATCHES "every step worked")
        message(FATAL_ERROR "editor --level-authoring on the ${copy} copy: exit ${code}, wanted ${expect_code}\n${out}\n${err}")
    endif()
    set(out "${out}" PARENT_SCOPE)
endfunction()

# ---- The good copy: everything the window made validates ----
author(good 0)
foreach(expected
        "new level \"level\" \\(Scenes/level.swscene\\)"
        "placed spawn \"start\", checkpoint \"checkpoint\" and an exit to \"menu\""
        "duplicated \"training\""
        "6 levels, 0 errors")
    if(NOT out MATCHES "${expected}")
        message(FATAL_ERROR "editor --level-authoring didn't say ${expected}:\n${out}")
    endif()
endforeach()
foreach(file Scenes/level.swscene Scenes/training-2.swscene)
    if(NOT EXISTS "${TEMP}/good/${file}")
        message(FATAL_ERROR "editor --level-authoring didn't write ${file}")
    endif()
endforeach()
file(READ "${TEMP}/good/Config/Levels.swlevels" catalog)
file(READ "${TEMP}/good/Scenes/level.swscene" scene)
if(NOT catalog MATCHES "level \"level\"" OR NOT catalog MATCHES "level \"training-2\"")
    message(FATAL_ERROR "the catalog doesn't list the new and duplicated levels:\n${catalog}")
endif()
foreach(component LevelSpawn LevelExit Checkpoint)
    if(NOT scene MATCHES "${component}")
        message(FATAL_ERROR "Scenes/level.swscene has no ${component}:\n${scene}")
    endif()
endforeach()
# The player agrees: the levels the editor wrote pass its whole check.
execute_process(COMMAND "${PLAYER}" --project "${TEMP}/good/ShippingDemo.swproject" --check
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "player --check on the levels the editor wrote failed (${code}):\n${out}\n${err}")
endif()

# ---- The broken copy: each problem under its kind ----
set(training "${TEMP}/broken/Scenes/training.swscene")
file(READ "${training}" text)
string(REPLACE "target \"Firing line\"" "target \"Nowhere\"" text "${text}")
string(REPLACE "Circuits/TrainingExit.swcircuit" "Circuits/Missing.swcircuit" text "${text}")
file(WRITE "${training}" "${text}")
set(factory "${TEMP}/broken/Scenes/factory.swscene")
file(READ "${factory}" text)
string(REGEX REPLACE "destination \"?ending\"?" "destination nowhere" text "${text}")
file(WRITE "${factory}" "${text}")

author(broken 1)
foreach(expected
        "content error: Scenes/training.swscene:[0-9]+: no entity named 'Nowhere'"
        "circuit error: Scenes/training.swscene: entity 'Training exit': Circuit: Circuits/Missing.swcircuit doesn't exist \\[CircuitMissing\\]"
        "gameplay error: Scenes/training.swscene: objective \"training.line\" \\(reach\\) needs a target with a Transform \\[GameContent\\]")
    if(NOT out MATCHES "${expected}")
        message(FATAL_ERROR "the editor's Validate didn't list: ${expected}\n${out}")
    endif()
endforeach()

execute_process(COMMAND "${PLAYER}" --project "${TEMP}/broken/ShippingDemo.swproject" --check
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 1)
    message(FATAL_ERROR "player --check on the broken copy: exit ${code}, wanted 1\n${out}\n${err}")
endif()
# Each heading, and its problem under it before the next heading.
foreach(expected
        "Missing or broken content \\([0-9]+, content\\):\n([^\n]+\n)*Scenes/training.swscene:[0-9]+: no entity named 'Nowhere'"
        "Circuit errors \\([0-9]+, circuit\\):\n([^\n]+\n)*Scenes/training.swscene: entity 'Training exit': Circuit: Circuits/Missing.swcircuit doesn't exist"
        "Gameplay code and rules \\([0-9]+, gameplay\\):\n([^\n]+\n)*Scenes/training.swscene: objective \"training.line\" \\(reach\\) needs a target"
        "Missing or broken content [^\n]*:\n([^\n]+\n)*[^\n]*exit[^\n]*\"nowhere\"")
    if(NOT out MATCHES "${expected}")
        message(FATAL_ERROR "player --check didn't list, under its kind: ${expected}\n${out}")
    endif()
endforeach()
if(out MATCHES "gameplay failed to start")
    message(FATAL_ERROR "player --check repeated the game's own findings as \"gameplay failed to start\":\n${out}")
endif()
message(STATUS "level authoring and failures by kind: passed")
