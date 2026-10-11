# GameUI: menus and HUDs from .rml/.rcss documents (RmlUi, FreeType, HarfBuzz), the
# UIDocument component and game code's UIWorld. See docs/GameUI.md and cmake/RmlUi.cmake.
set(harfbuzz_engine
    FontEngineInterfaceHarfBuzz.cpp FontFace.cpp FontFaceHandleHarfBuzz.cpp FontFaceLayer.cpp FontFamily.cpp
    FontProvider.cpp FreeTypeInterface.cpp TextureLayout.cpp TextureLayoutRectangle.cpp TextureLayoutRow.cpp
    TextureLayoutTexture.cpp)
list(TRANSFORM harfbuzz_engine PREPEND ${SHOCKWAVE_RMLUI_HARFBUZZ_DIR}/)
shockwave_module(GameUI
    PUBLIC_DEPS Shockwave::Engine
    PRIVATE_DEPS rmlui_core harfbuzz Freetype::Freetype
    SOURCES ${harfbuzz_engine}
    NO_WARNINGS ${harfbuzz_engine})
# The sample engine uses a few of RmlUi's private Core headers.
target_include_directories(ShockwaveGameUI SYSTEM PRIVATE ${SHOCKWAVE_RMLUI_HARFBUZZ_DIR} ${SHOCKWAVE_RMLUI_PRIVATE_DIR})
