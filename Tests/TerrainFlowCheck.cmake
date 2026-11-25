# Terrain in a real game (W1a): a copy of Reactor Run with a terrain added to its training
# level (well below the floor, out of the way), checked by --check and played through by the
# game's own whole-run flow test. The terrain loads with the level, collides and unloads.
# cmake -DPLAYER=... -DGAME=Games/ShippingDemo -DTERRAIN=Tests/fixtures/graphics/Content/terrain
#       -DTEMP=... -P TerrainFlowCheck.cmake
foreach(var PLAYER GAME TERRAIN TEMP)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "TerrainFlowCheck.cmake needs -D${var}=...")
    endif()
endforeach()
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}/game")
foreach(part ShippingDemo.swproject Config Content Scenes Tests)
    file(COPY "${GAME}/${part}" DESTINATION "${TEMP}/game")
endforeach()
file(COPY "${TERRAIN}/hills.swterrain" "${TERRAIN}/hills.r16" DESTINATION "${TEMP}/game/Content/terrain")
file(APPEND "${TEMP}/game/Scenes/training.swscene" "
entity \"Terrain\"
  Transform
    position -256 -80 -256
  Terrain
    asset terrain/hills.swterrain
  RigidBody
    motion Static
  Collider
    shape Terrain
end
")

function(player_run name)
    execute_process(COMMAND "${PLAYER}" --project "${TEMP}/game/ShippingDemo.swproject" ${ARGN}
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "${name} failed (${code}):\n${out}\n${err}")
    endif()
    set(out "${out}" PARENT_SCOPE)
endfunction()
player_run("--check" --check)
player_run("--asset-dependencies" --asset-dependencies --scene "${TEMP}/game/Scenes/training.swscene")
if(NOT out MATCHES "terrain/hills.swterrain" OR NOT out MATCHES "terrain/hills.r16")
    message(FATAL_ERROR "the training level's dependencies don't list the terrain and its heights:\n${out}")
endif()
player_run("the whole game" --renderer null --user-dir "${TEMP}/user"
    --flow-test "${TEMP}/game/Tests/flow/full-run.json" --json "${TEMP}/full-run.json")

# --check catches a broken terrain: its heights cut short.
file(WRITE "${TEMP}/game/Content/terrain/hills.r16" "short")
execute_process(COMMAND "${PLAYER}" --project "${TEMP}/game/ShippingDemo.swproject" --check
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(code EQUAL 0 OR NOT "${out}${err}" MATCHES "hills.r16 has 5 bytes")
    message(FATAL_ERROR "--check passed a terrain with a short heights file (${code}):\n${out}\n${err}")
endif()
message(STATUS "terrain: Reactor Run's training level with a terrain checks and plays through")
