# Foliage painting in the editor (W2b, docs/Foliage.md#painting): on a copy of the graphics
# fixtures, a scene with flat ground (a static box collider) and an empty Foliage; `editor
# --foliage-stroke` paints one stroke with the Foliage window's own code, undoes and redoes
# it, erases part of it, undoes that, and saves. Then the saved file is checked here.
#   -DEDITOR=editor -DPLAYER=player -DFIXTURE=Tests/fixtures/graphics -DTEMP=dir
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}")
file(COPY "${FIXTURE}/" DESTINATION "${TEMP}/graphics")
set(project "${TEMP}/graphics")
file(WRITE "${project}/Content/foliage/paint.swfoliage" "swfoliage 2
instances \"foliage/paint.instances\"
type \"models/grass-tuft.glb\"
  distance 60
  fade 10
  shadows false
  wind 0
  max_slope 45
  scale_min 0.8
  scale_max 1.2
")
file(WRITE "${project}/Content/foliage/paint.instances" "")
file(WRITE "${project}/Scenes/foliage-paint.swscene" "shockwave_scene 2

entity \"Ground\"
  Transform
    position 0 -0.5 0
  RigidBody
    motion Static
  Collider
    shape Box
    half_extents 50 0.5 50
end

entity \"Meadow\"
  Transform
  Foliage
    asset foliage/paint.swfoliage
end
")
execute_process(COMMAND "${EDITOR}" "${project}/Scenes/foliage-paint.swscene" --prefs "${TEMP}/prefs" --foliage-stroke
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0 OR NOT out MATCHES "foliage stroke: painted ([0-9]+), erased ([0-9]+); undo, redo and save agree")
    message(FATAL_ERROR "editor --foliage-stroke: exit ${result}\n${out}\n${err}")
endif()
set(painted "${CMAKE_MATCH_1}")
set(erased "${CMAKE_MATCH_2}")
# Three dabs of a 4 m circle 1 m apart at 2 per square metre: about 2 * (the union's area).
if(painted LESS 100 OR painted GREATER 200)
    message(FATAL_ERROR "the stroke painted ${painted} instances, not about 140")
endif()
execute_process(COMMAND "${PLAYER}" --foliage-info "${project}/Content/foliage/paint.swfoliage"
                RESULT_VARIABLE result OUTPUT_VARIABLE info ERROR_VARIABLE err)
if(NOT result EQUAL 0 OR NOT info MATCHES "${painted} instances")
    message(FATAL_ERROR "the saved foliage doesn't hold the ${painted} painted instances:\n${info}\n${err}")
endif()
message(STATUS "foliage painting: ${painted} painted, ${erased} erased and undone, saved")
