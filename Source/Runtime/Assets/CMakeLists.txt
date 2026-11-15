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
