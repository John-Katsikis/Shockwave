# Assets: the asset registry, image decoding, model import and the hot-reloading asset folder.
include(${PROJECT_SOURCE_DIR}/cmake/AssetOptimisation.cmake)   # meshoptimizer, Basis Universal
shockwave_module(Assets
    PUBLIC_DEPS Shockwave::Renderer Shockwave::Projects
    PRIVATE_DEPS assimp::assimp meshoptimizer basisu
    NO_WARNINGS ${CMAKE_CURRENT_SOURCE_DIR}/Private/Image.cpp ${CMAKE_CURRENT_SOURCE_DIR}/Private/Vorbis.cpp)
target_include_directories(ShockwaveAssets SYSTEM PRIVATE ${stb_image_SOURCE_DIR} ${stb_vorbis_SOURCE_DIR})
target_include_directories(ShockwaveAssets SYSTEM PRIVATE ${assimp_SOURCE_DIR}/contrib/rapidjson/include)   # glTF JSON the importer does not pass on
# rapidjson is header-only: compile it exactly as Assimp does (code/CMakeLists.txt), or the
# linker may hand Assimp our copy of an inline function with another iterator type and
# calling convention. The nightly sanitizer job found this as a crash in glTF loading.
target_compile_definitions(ShockwaveAssets PRIVATE RAPIDJSON_HAS_STDSTRING=1 RAPIDJSON_NOMEMBERITERATORCLASS)

# The optional Autodesk FBX SDK importer (-DSHOCKWAVE_FBXSDK=ON, cmake/FbxSdk.cmake). Without
# it FbxSdkImport.cpp isn't compiled and Assimp reads .fbx files.
if(SHOCKWAVE_FBXSDK)
    target_link_libraries(ShockwaveAssets PRIVATE FbxSdk::fbxsdk)
    target_compile_definitions(ShockwaveAssets PRIVATE SHOCKWAVE_HAS_FBXSDK=1)
else()
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/Private/FbxSdkImport.cpp
                                TARGET_DIRECTORY ShockwaveAssets PROPERTIES HEADER_FILE_ONLY TRUE)
endif()

# Material graphs compile at edit time (Assets/ShaderCompiler.h) with the glslangValidator the
# engine's shaders were built with, against the engine's shader sources. A packaged game only
# reads the compiled cache and never uses either.
target_compile_definitions(ShockwaveAssets PRIVATE
    SHOCKWAVE_GLSLANG_PATH="${SHOCKWAVE_GLSLANG_COMMAND}"
    SHOCKWAVE_SHADER_SOURCE_DIR="${PROJECT_SOURCE_DIR}/Source/Plugins/RendererVulkan/Shaders")
if(SHOCKWAVE_GLSLANG_DEPENDS)
    add_dependencies(ShockwaveAssets ${SHOCKWAVE_GLSLANG_DEPENDS})
endif()
