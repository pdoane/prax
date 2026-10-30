# Fonts: the fonts of font files and the data that drawing needs from them, and the backends that the build turns on,
# which hint glyphs and list the system's fonts.
pr_library(font INCLUDE include
    src/cff.cpp
    src/colr.cpp
    src/font.cpp
    src/font_hinter.cpp
    src/font_set.cpp
    src/glyf.cpp
    src/glyph_metrics.cpp
    src/outline.cpp
    src/sfnt.cpp
    src/woff.cpp
)
target_link_libraries(pr_font PUBLIC prax::path prax::compress prax::geom prax::locale prax::color PRIVATE prax::core_cpp)

# FreeType, without any of the optional libraries that it can use, and the backend of font hinters built on it.
if (PR_FONT_FREETYPE)
    set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
    pr_dependency_available(freetype)
    pr_third_party_directory(${freetype_SOURCE_DIR})
    set_property(GLOBAL APPEND PROPERTY PR_LIBRARIES freetype)

    target_sources(pr_font PRIVATE backends/freetype/src/font_freetype.cpp)
    pr_headers(pr_font PUBLIC backends/freetype/include)
    target_compile_definitions(pr_font PRIVATE PR_FONT_HAS_FREETYPE)
    target_link_libraries(pr_font PRIVATE freetype)
endif()

# The backends of the system's fonts.
if (PR_FONT_CORETEXT)
    target_sources(pr_font PRIVATE backends/coretext/src/font_coretext.cpp)
    pr_headers(pr_font PUBLIC backends/coretext/include)
    target_compile_definitions(pr_font PRIVATE PR_FONT_HAS_CORETEXT)
    target_link_libraries(pr_font PUBLIC "-framework CoreText" "-framework CoreFoundation")
endif()
if (PR_FONT_DIRECTWRITE)
    target_sources(pr_font PRIVATE backends/directwrite/src/font_directwrite.cpp)
    pr_headers(pr_font PUBLIC backends/directwrite/include)
    target_compile_definitions(pr_font PRIVATE PR_FONT_HAS_DIRECTWRITE)
    target_link_libraries(pr_font PUBLIC dwrite)
endif()

if (PR_DEVELOP)
    pr_module_tests(font
        tests/test_font.cpp
        tests/test_font_c.c
        tests/test_font_set.cpp
        tests/test_outline.cpp
        tests/test_woff.cpp
    )
    target_link_libraries(font_tests PRIVATE prax::compress prax::path freetype)
    target_compile_definitions(font_tests PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")
    if (PR_FONT_FREETYPE)
        target_sources(font_tests PRIVATE
            backends/freetype/tests/test_font_freetype.cpp
            backends/freetype/tests/test_font_freetype_c.c
        )
    endif()
    if (PR_FONT_CORETEXT)
        target_sources(font_tests PRIVATE
            backends/coretext/tests/test_font_coretext.cpp
            backends/coretext/tests/test_font_coretext_c.c
        )
    endif()
    if (PR_FONT_DIRECTWRITE)
        target_sources(font_tests PRIVATE
            backends/directwrite/tests/test_font_directwrite.cpp
            backends/directwrite/tests/test_font_directwrite_c.c
        )
    endif()
endif()
