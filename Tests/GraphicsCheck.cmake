file(MAKE_DIRECTORY "${TEMP}")
file(WRITE "${TEMP}/good.swgraphics" "swgraphics 1\nrender_scale 0.5\nshadow_quality low\nframe_limit 60\n")
execute_process(COMMAND "${PLAYER}" --project "${PROJECT}" --graphics "${TEMP}/good.swgraphics" --check
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Valid graphics failed --check: ${out}\n${err}")
endif()
execute_process(COMMAND "${PLAYER}" --project "${PROJECT}" --graphics "${TEMP}/good.swgraphics" --render-scale 0.75 --fps 120 --graphics-info
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0 OR NOT out MATCHES "render_scale 0.75" OR NOT out MATCHES "frame_limit 120" OR NOT out MATCHES "shadow_quality low")
    message(FATAL_ERROR "CLI overrides failed: ${out}\n${err}")
endif()
foreach(value "render_scale nan" "shadow_quality invalid" "frame_limit 9" "lod_error_pixels -1" "vsync wrong" "low_latency 1"
        "anti_aliasing taa" "anti_aliasing smaa" "bloom_quality ultra" "bloom_scale 2" "bloom_scale nan"
        "texture_quality extreme" "texture_quality 0" "anisotropy 3" "anisotropy 32" "anisotropy 0" "material_quality medium")
    file(WRITE "${TEMP}/bad.swgraphics" "swgraphics 1\n${value}\n")
    execute_process(COMMAND "${PLAYER}" --project "${PROJECT}" --graphics "${TEMP}/bad.swgraphics" --check
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 1 OR NOT "${out}${err}" MATCHES "bad.swgraphics:2:")
        message(FATAL_ERROR "Invalid graphics accepted: ${value}: ${out}\n${err}")
    endif()
endforeach()
# The shared settings table (A00): --graphics-set reaches every key, --anti-aliasing is a
# shortcut for one of them, TAA is refused until it exists, and --json reports the result.
execute_process(COMMAND "${PLAYER}" --project "${PROJECT}" --graphics "${TEMP}/good.swgraphics"
    --graphics-set bloom_quality=low --graphics-set bloom_scale=0.5 --anti-aliasing fxaa --graphics-info
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0 OR NOT out MATCHES "anti_aliasing fxaa" OR NOT out MATCHES "bloom_quality low"
   OR NOT out MATCHES "bloom_scale 0.5" OR NOT out MATCHES "preset: custom")
    message(FATAL_ERROR "--graphics-set/--anti-aliasing failed: ${out}\n${err}")
endif()
foreach(args "--anti-aliasing;taa" "--graphics-set;bloom_scale=3" "--graphics-set;banana=1" "--graphics-set;vsync")
    execute_process(COMMAND "${PLAYER}" --project "${PROJECT}" ${args} --graphics-info
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(result EQUAL 0)
        message(FATAL_ERROR "Invalid override accepted: ${args}: ${out}\n${err}")
    endif()
endforeach()
execute_process(COMMAND "${PLAYER}" --project "${PROJECT}" --anti-aliasing taa --graphics-info
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT "${out}${err}" MATCHES "taa")
    message(FATAL_ERROR "Refusing TAA should say why: ${out}\n${err}")
endif()
execute_process(COMMAND "${PLAYER}" --project "${PROJECT}" --graphics "${TEMP}/good.swgraphics" --anti-aliasing fxaa
    --graphics-info --json "${TEMP}/info.json" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "--graphics-info --json failed: ${out}\n${err}")
endif()
file(READ "${TEMP}/info.json" info)
string(JSON aa GET "${info}" settings anti_aliasing)
string(JSON scale GET "${info}" settings render_scale)
if(NOT aa STREQUAL "fxaa" OR NOT scale STREQUAL "0.5")
    message(FATAL_ERROR "--graphics-info --json doesn't match: ${info}")
endif()
file(REMOVE_RECURSE "${TEMP}")
