# The graphics overhaul in a package (G13/A10, docs/Graphics/06-Verification.md 6.10): a
# copy of the graphics fixtures is baked (lightmaps and probes), has its reflections
# captured, and is packaged through the real CLI. The packaged game, run from outside the
# source tree, must then render every graphics feature exactly as the project it came
# from: baked lighting, reflection probes, layered materials, colour grading with a LUT,
# lit particles, decals, glass, skin and the 6v6 combat scene. Playing must write nothing
# into the package: no bake, compression or cache on a player's first launch.
cmake_policy(SET CMP0054 NEW)
file(REMOVE_RECURSE "${TEMP}")
file(MAKE_DIRECTORY "${TEMP}/images")
set(PROJECT "${TEMP}/project")
file(COPY "${FIXTURE}/" DESTINATION "${PROJECT}")
function(run what)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err
        WORKING_DIRECTORY "${TEMP}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${what} failed (${result}):\n${out}\n${err}")
    endif()
    set(out "${out}" PARENT_SCOPE)
endfunction()
run("--bake-lighting" "${PLAYER}" --project "${PROJECT}/Graphics.swproject" --bake-lighting
    --scene "${PROJECT}/Scenes/room-doorway-baked.swscene" --quality preview --seed 7)
run("--capture-reflections" "${PLAYER}" --project "${PROJECT}/Graphics.swproject" --capture-reflections
    --scene "${PROJECT}/Scenes/reflection-rooms.swscene")
run("--package" "${PLAYER}" --package --project "${PROJECT}/Graphics.swproject" --no-build --no-archive
    --output "${TEMP}/out")
if(NOT out MATCHES "package ([^\n]+)")
    message(FATAL_ERROR "player --package didn't say where the package is:\n${out}")
endif()
set(package "${CMAKE_MATCH_1}")
foreach(file Game/Content/Lighting/room-doorway-baked.swlighting Game/Content/Lighting/reflection-rooms.swreflection
        Game/Content/Lighting/room-doorway-baked/Lightmap_000.ktx2 Game/Content/postprocess/luts/warm.cube
        Game/Content/effects/lit-smoke.efkefc Game/Content/materials/shading/skin.swmaterial)
    if(NOT EXISTS "${package}/${file}")
        message(FATAL_ERROR "The package is missing ${file}")
    endif()
endforeach()

file(GLOB_RECURSE before "${package}/*")
set(before_hashes "")
foreach(file ${before})
    file(SHA256 "${file}" hash)
    string(APPEND before_hashes "${file} ${hash}\n")
endforeach()

foreach(scene room-doorway-baked reflection-rooms material-layers postprocess-chart particles-lit decal-wall
        shading-glass shading-skin combat-6v6)
    set(common --no-user-graphics --size 320x180 --frames 8 --fixed-step --graphics-preset high)
    run("${scene} from the project" "${CMAKE_COMMAND}" -E env SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy
        "${PLAYER}" --project "${PROJECT}/Graphics.swproject" --scene "${PROJECT}/Scenes/${scene}.swscene" ${common}
        --screenshot "${TEMP}/images/${scene}-project.png")
    run("${scene} from the package" "${CMAKE_COMMAND}" -E env SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy
        "${package}/Binaries/Graphicsfixtures" --scene "${package}/Game/Scenes/${scene}.swscene" ${common}
        --screenshot "${TEMP}/images/${scene}-package.png")
    execute_process(COMMAND "${PLAYER}" --image-diff "${TEMP}/images/${scene}-project.png"
        "${TEMP}/images/${scene}-package.png" --tolerance 255 --json "${TEMP}/diff.json" OUTPUT_QUIET)
    file(READ "${TEMP}/diff.json" diff)
    string(JSON rmse GET "${diff}" rmse)
    if(NOT rmse EQUAL 0)
        message(FATAL_ERROR "${scene}: the package renders it differently (RMSE ${rmse})")
    endif()
endforeach()

set(after_hashes "")
file(GLOB_RECURSE after "${package}/*")
foreach(file ${after})
    file(SHA256 "${file}" hash)
    string(APPEND after_hashes "${file} ${hash}\n")
endforeach()
if(NOT before_hashes STREQUAL after_hashes)
    message(FATAL_ERROR "Playing changed the package (a first-launch bake, compression or cache?)")
endif()
message(STATUS "9 scenes render the same from the package; playing left it unchanged")
