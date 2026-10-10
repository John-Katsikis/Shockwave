# Assets: the asset registry, image decoding, model import and the hot-reloading asset folder.
shockwave_module(Assets
    PUBLIC_DEPS Shockwave::Renderer
    PRIVATE_DEPS assimp::assimp
    NO_WARNINGS ${CMAKE_CURRENT_SOURCE_DIR}/Private/Image.cpp ${CMAKE_CURRENT_SOURCE_DIR}/Private/Vorbis.cpp)
target_include_directories(ShockwaveAssets SYSTEM PRIVATE ${stb_image_SOURCE_DIR} ${stb_vorbis_SOURCE_DIR})
