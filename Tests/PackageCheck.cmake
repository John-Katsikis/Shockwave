# Packages the sample through the real CLI and checks the result stands on its own:
# the files players need are there (textures compressed), developer files aren't, and
# the packaged player passes its own --check. No build (ctest runs inside the build) and no archive.
file(REMOVE_RECURSE "${TEMP}")
execute_process(COMMAND "${PLAYER}" --package --project "${PROJECT}" --no-build --no-archive --output "${TEMP}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0 OR NOT output MATCHES "package ([^\n]+)")
    message(FATAL_ERROR "player --package failed (${result}):\n${output}\n${errors}")
endif()
set(package "${CMAKE_MATCH_1}")
foreach(file ShockwaveSample README.txt Binaries/ShockwaveSample Binaries/shockwave-package.txt
        Binaries/modules/game_sample.so Binaries/plugins/renderer_vulkan.so Game/Sample.swproject
        Game/Scenes/main.swscene Game/Config/Input.swinput Licenses/Shockwave-LICENSE.txt)
    if(NOT EXISTS "${package}/${file}")
        message(FATAL_ERROR "The package is missing ${file}")
    endif()
endforeach()
foreach(file Game/Source Game/Tools Game/Saved/SaveGames Game/CMakeLists.txt Game/.gitignore)
    if(EXISTS "${package}/${file}")
        message(FATAL_ERROR "The package shouldn't ship ${file}")
    endif()
endforeach()
# Textures ship compressed, so players' machines don't compress them; nothing else in
# Saved/ ships.
file(GLOB shipped_textures "${package}/Game/Saved/Cache/Textures/*.ktx2")
file(GLOB_RECURSE shipped_saved RELATIVE "${package}/Game/Saved" "${package}/Game/Saved/*")
list(LENGTH shipped_textures texture_count)
list(FILTER shipped_saved EXCLUDE REGEX "^Cache/Textures/[^/]+\\.ktx2$")
if(texture_count EQUAL 0 OR shipped_saved)
    message(FATAL_ERROR "The package should ship compressed textures and nothing else from Saved: ${shipped_saved}")
endif()
# Started from elsewhere, the packaged player still finds its own project.
execute_process(COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy
        "${package}/Binaries/ShockwaveSample" --check
    WORKING_DIRECTORY "${TEMP}" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0 OR NOT output MATCHES "0 problems")
    message(FATAL_ERROR "The packaged game's --check failed (${result}):\n${output}\n${errors}")
endif()
# A broken settings file stops packaging with the file and line.
set(project_copy "${TEMP}/Project")
get_filename_component(project_dir "${PROJECT}" DIRECTORY)
file(COPY "${project_dir}/Sample.swproject" "${project_dir}/Scenes" DESTINATION "${project_copy}")
file(COPY "${project_dir}/Content/Circuits" DESTINATION "${project_copy}/Content")   # game_circuit must exist
file(WRITE "${project_copy}/Config/Package.swpackage" "shockwave_package 1\nicon \"x.png\"\n")
execute_process(COMMAND "${PLAYER}" --package --project "${project_copy}/Sample.swproject" --no-build
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(result EQUAL 0 OR NOT errors MATCHES "Package.swpackage:2: unknown key 'icon'")
    message(FATAL_ERROR "A broken settings file should fail (${result}):\n${output}\n${errors}")
endif()
file(REMOVE_RECURSE "${TEMP}")
