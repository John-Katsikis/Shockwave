# Game UI (Source/Runtime/GameUI, docs/GameUI.md): RmlUi lays out HTML/CSS-like documents,
# FreeType rasterizes glyphs and HarfBuzz shapes text (kerning, ligatures, Arabic,
# Devanagari...). All three are static libraries linked into the GameUI module only, built
# without optional system dependencies so no distro package is needed.

# FreeType: no zlib, bzip2, PNG, Brotli or HarfBuzz autohinting, so it needs nothing else.
foreach(feature ZLIB BZIP2 PNG HARFBUZZ BROTLI)
    set(FT_DISABLE_${feature} ON CACHE BOOL "" FORCE)
endforeach()
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(freetype
    GIT_REPOSITORY https://github.com/freetype/freetype.git
    GIT_TAG VER-2-14-3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(freetype)
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

# HarfBuzz: the amalgamated library with FreeType glue (hb-ft), nothing more.
set(HB_HAVE_FREETYPE ON CACHE BOOL "" FORCE)
set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(harfbuzz
    GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
    GIT_TAG 11.5.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(harfbuzz)

# RmlUi with its FreeType font engine; GameUI swaps in the HarfBuzz shaping engine from
# RmlUi's own sample (Samples/basic/harfbuzz), compiled straight from the fetched sources.
set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(RMLUI_THIRDPARTY_CONTAINERS ON CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_Freetype ON)   # use the FreeType built above, never a system one
FetchContent_Declare(rmlui
    GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
    GIT_TAG 6.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(rmlui)
unset(CMAKE_DISABLE_FIND_PACKAGE_Freetype)
foreach(target freetype harfbuzz rmlui_core)
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE -w)
    endif()
    set_target_properties(${target} PROPERTIES FOLDER "ThirdParty")
endforeach()

# The HarfBuzz font engine from RmlUi's sample, built into the GameUI module.
set(SHOCKWAVE_RMLUI_HARFBUZZ_DIR ${rmlui_SOURCE_DIR}/Samples/basic/harfbuzz/src)
set(SHOCKWAVE_RMLUI_PRIVATE_DIR ${rmlui_SOURCE_DIR}/Source/Core)

# The default UI font (Noto Sans, SIL Open Font License 1.1; see NotoSans-OFL.txt), copied
# next to the executables like the editor's fonts (build/bin/fonts), and the three
# libraries' licenses (build/bin/licenses).
file(COPY ${CMAKE_SOURCE_DIR}/Source/Runtime/GameUI/Fonts/NotoSans-Regular.ttf
          ${CMAKE_SOURCE_DIR}/Source/Runtime/GameUI/Fonts/NotoSans-Bold.ttf
     DESTINATION ${CMAKE_BINARY_DIR}/bin/fonts)
configure_file(${CMAKE_SOURCE_DIR}/Source/Runtime/GameUI/Fonts/OFL.txt ${CMAKE_BINARY_DIR}/bin/fonts/NotoSans-OFL.txt
               COPYONLY)
configure_file(${rmlui_SOURCE_DIR}/LICENSE.txt ${CMAKE_BINARY_DIR}/bin/licenses/RmlUi-LICENSE.txt COPYONLY)
configure_file(${freetype_SOURCE_DIR}/LICENSE.TXT ${CMAKE_BINARY_DIR}/bin/licenses/FreeType-LICENSE.txt COPYONLY)
configure_file(${freetype_SOURCE_DIR}/docs/FTL.TXT ${CMAKE_BINARY_DIR}/bin/licenses/FreeType-FTL.txt COPYONLY)
configure_file(${harfbuzz_SOURCE_DIR}/COPYING ${CMAKE_BINARY_DIR}/bin/licenses/HarfBuzz-COPYING.txt COPYONLY)
