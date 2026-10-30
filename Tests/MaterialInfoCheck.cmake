# Material bindings and texture info without a GPU (G02, A01): what the importer makes of
# the fixture's materials.glb, the UV1 rule in --check, and texture sizes at each quality.
file(MAKE_DIRECTORY "${TEMP}")
function(material_info asset out_var)
    execute_process(COMMAND "${PLAYER}" --material-info "${asset}" --json "${TEMP}/info.json"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "--material-info ${asset} failed: ${out}\n${err}")
    endif()
    file(READ "${TEMP}/info.json" json)
    set(${out_var} "${json}" PARENT_SCOPE)
endfunction()
# expect(json, material index, slot index, member path..., value)
function(expect json material slot value)
    string(JSON actual GET "${json}" materials ${material} slots ${slot} ${ARGN})
    string(JSON type TYPE "${json}" materials ${material} slots ${slot} ${ARGN})
    if(type STREQUAL "NUMBER")
        string(JSON same EQUAL "${actual}" "${value}")   # as numbers: 0.3 is 0.29999999999999999
    else()
        set(same FALSE)
        if("${actual}" STREQUAL "${value}")
            set(same TRUE)
        endif()
    endif()
    if(NOT same)
        string(JSON name GET "${json}" materials ${material} name)
        message(FATAL_ERROR "material '${name}' slot ${slot} ${ARGN}: expected ${value}, got ${actual}")
    endif()
endfunction()

material_info("${FIXTURE}/Content/models/materials.glb" info)
string(JSON warnings LENGTH "${info}" warnings)
if(NOT warnings EQUAL 0)
    message(FATAL_ERROR "materials.glb imports with warnings: ${info}")
endif()
# 0 Transform: KHR_texture_transform comes back as authored (Assimp hands it over changed).
expect("${info}" 0 0 "0.3" transform offset 0)
expect("${info}" 0 0 "-0.2" transform offset 1)
expect("${info}" 0 0 "1.5" transform scale 0)
expect("${info}" 0 0 "-0.8" transform scale 1)
expect("${info}" 0 0 "0.7" transform rotation)
# 1 UV1, 2 KHR_texture_transform's own texCoord overriding the texture's.
expect("${info}" 1 0 "1" uv_set)
expect("${info}" 2 0 "1" uv_set)
expect("${info}" 2 0 "0.25" transform offset 0)
# 3 a rotated, mirrored normal map; 4 a normal map on UV1; colour spaces.
expect("${info}" 3 0 "normal" slot)
expect("${info}" 3 0 "-1" transform scale 0)
expect("${info}" 3 0 "1.1" transform rotation)
expect("${info}" 3 0 "linear" color_space)
expect("${info}" 4 0 "1" uv_set)
expect("${info}" 0 0 "srgb" color_space)
# 6 sampler modes: clamp U, mirror V, nearest without mipmaps.
expect("${info}" 6 0 "clamp" sampler wrap_u)
expect("${info}" 6 0 "mirror" sampler wrap_v)
expect("${info}" 6 0 "nearest" sampler mag_filter)
expect("${info}" 6 0 "nearest" sampler min_filter)
expect("${info}" 6 0 "none" sampler mip_filter)
expect("${info}" 0 0 "linear" sampler mip_filter)
# 7 different UV sets per slot of one material.
expect("${info}" 7 0 "0" uv_set)
expect("${info}" 7 1 "emissive" slot)
expect("${info}" 7 1 "1" uv_set)

# A material asking for a UV set the mesh lacks: a warning on import, and --check fails.
material_info("${FIXTURE}/Content/models/missing-uv1.glb" info)
expect("${info}" 0 0 "OFF" uv_available)   # CMake reads JSON false as OFF
string(JSON warning GET "${info}" warnings 0)
if(NOT warning MATCHES "no second UV set")
    message(FATAL_ERROR "missing-uv1.glb should warn about TEXCOORD_1: ${info}")
endif()
file(WRITE "${TEMP}/uv1-check.swscene"
    "shockwave_scene 2\nentity \"Model\"\n  Transform\n  ModelRenderer\n    model models/missing-uv1.glb\nend\n"
    "entity \"Camera\"\n  Transform\n  Camera\n    primary 1\nend\n")
execute_process(COMMAND "${PLAYER}" --project "${FIXTURE}/Graphics.swproject" --check "${TEMP}/uv1-check.swscene"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(result EQUAL 0 OR NOT "${out}${err}" MATCHES "reads its base_color map from UV1")
    message(FATAL_ERROR "--check didn't flag a map read from a missing UV1 (exit ${result}):\n${out}\n${err}")
endif()

# --texture-info: sizes and bytes at each quality, without a GPU.
execute_process(COMMAND "${PLAYER}" --texture-info "${FIXTURE}/Content/textures/checker-fine.png"
    --json "${TEMP}/texture.json" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "--texture-info failed: ${out}\n${err}")
endif()
file(READ "${TEMP}/texture.json" texture)
string(JSON width GET "${texture}" textures 0 width)
string(JSON ultra GET "${texture}" textures 0 quality ultra bytes)
string(JSON low_mip GET "${texture}" textures 0 quality low first_mip)
string(JSON low_width GET "${texture}" textures 0 quality low width)
string(JSON low GET "${texture}" textures 0 quality low bytes)
# 256 x 256 RGBA8: the whole chain is 349,524 bytes; from 32 x 32 down, 5,460.
if(NOT width EQUAL 256 OR NOT ultra EQUAL 349524 OR NOT low_mip EQUAL 3 OR NOT low_width EQUAL 32 OR NOT low EQUAL 5460)
    message(FATAL_ERROR "--texture-info sizes are wrong: ${texture}")
endif()
file(REMOVE_RECURSE "${TEMP}")
