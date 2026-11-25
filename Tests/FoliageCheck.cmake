# Foliage in a real game (W2a): a copy of Reactor Run with the graphics fixtures' meadow
# added to its training level, through --check and --asset-dependencies; then --check
# catches each broken foliage: instances not a multiple of 24 bytes, a type the header
# doesn't have, and a type whose model isn't there.
# cmake -DPLAYER=... -DGAME=Games/ShippingDemo -DFIXTURE=Tests/fixtures/graphics -DTEMP=...
#       -P FoliageCheck.cmake
foreach(var PLAYER GAME FIXTURE TEMP)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "FoliageCheck.cmake needs -D${var}=...")
    endif()
endforeach()
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}/game")
foreach(part ShippingDemo.swproject Config Content Scenes)
    file(COPY "${GAME}/${part}" DESTINATION "${TEMP}/game")
endforeach()
set(content "${TEMP}/game/Content")
file(COPY "${FIXTURE}/Content/foliage/meadow.swfoliage" "${FIXTURE}/Content/foliage/meadow.instances"
     DESTINATION "${content}/foliage")
file(COPY "${FIXTURE}/Content/models/grass-tuft.glb" "${FIXTURE}/Content/models/pebble.glb"
     DESTINATION "${content}/models")
file(APPEND "${TEMP}/game/Scenes/training.swscene" "
entity \"Meadow\"
  Transform
    position 0 -80 0
  Foliage
    asset foliage/meadow.swfoliage
end
")

function(check expect_ok pattern why)
    execute_process(COMMAND "${PLAYER}" --project "${TEMP}/game/ShippingDemo.swproject" --renderer null --check
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(expect_ok AND NOT code EQUAL 0)
        message(FATAL_ERROR "--check failed with the meadow (${code}):\n${out}\n${err}")
    endif()
    if(NOT expect_ok AND (code EQUAL 0 OR NOT "${out}${err}" MATCHES "${pattern}"))
        message(FATAL_ERROR "--check didn't catch ${why} (${code}):\n${out}\n${err}")
    endif()
endfunction()
check(TRUE "" "")
execute_process(COMMAND "${PLAYER}" --project "${TEMP}/game/ShippingDemo.swproject" --asset-dependencies
    --scene "${TEMP}/game/Scenes/training.swscene" RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
foreach(file foliage/meadow.swfoliage foliage/meadow.instances models/grass-tuft.glb models/pebble.glb)
    if(NOT out MATCHES "${file}")
        message(FATAL_ERROR "the training level's dependencies don't list ${file}:\n${out}\n${err}")
    endif()
endforeach()

file(READ "${FIXTURE}/Content/foliage/meadow.swfoliage" header)
# 24 bytes an instance: one byte more is broken.
file(WRITE "${content}/foliage/meadow.instances" "0123456789012345678901234")
check(FALSE "not a multiple of 24" "an instances file of 25 bytes")
# One instance of type 7, which the two-type header doesn't have (Tools/foliage.py wrote it
# outside Content, so the fixture project never loads it).
file(COPY_FILE "${FIXTURE}/Broken/type-7.instances" "${content}/foliage/meadow.instances")
check(FALSE "instance 0 is type 7" "an instance of a type the header doesn't have")
# A type whose model isn't in Content.
file(COPY "${FIXTURE}/Content/foliage/meadow.instances" DESTINATION "${content}/foliage")
string(REPLACE "models/pebble.glb" "models/boulder.glb" missing "${header}")
file(WRITE "${content}/foliage/meadow.swfoliage" "${missing}")
check(FALSE "type 1's model models/boulder.glb isn't loaded" "a type whose model is missing")
message(STATUS "foliage: Reactor Run's training level with a meadow checks; --check catches each broken foliage")
