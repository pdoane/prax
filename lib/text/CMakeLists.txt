# Text shaping: the interface, and the backends that the build turns on.
pr_library(text INCLUDE include
    src/text.cpp
    src/text_break.cpp
    src/text_estimate.cpp
    src/text_font.cpp
    src/text_grid.cpp
    src/text_layout.cpp
    src/text_shaping.cpp
    src/text_transform.cpp
    src/text_itemize.cpp
    src/font_cache.cpp
)
target_link_libraries(pr_text PUBLIC prax::font prax::geom prax::unicode prax::locale PRIVATE prax::core_cpp)

if (PR_TEXT_KB)
    pr_dependency_available(kb)
    target_sources(pr_text PRIVATE backends/kb/src/text_kb.cpp)
    pr_headers(pr_text PUBLIC backends/kb/include)
    target_include_directories(pr_text SYSTEM PRIVATE ${kb_SOURCE_DIR})
    target_compile_definitions(pr_text PRIVATE PR_TEXT_HAS_KB)
endif()

if (PR_TEXT_SIMPLE)
    target_sources(pr_text PRIVATE backends/simple/src/text_simple.cpp)
    pr_headers(pr_text PUBLIC backends/simple/include)
    target_compile_definitions(pr_text PRIVATE PR_TEXT_HAS_SIMPLE)
endif()

# HarfBuzz, built as its single-source amalgamation, and the backend built on it.
if (PR_TEXT_HARFBUZZ)
    pr_dependency_available(harfbuzz)
    add_library(pr_harfbuzz STATIC ${harfbuzz_SOURCE_DIR}/src/harfbuzz.cc)
    add_library(prax::harfbuzz ALIAS pr_harfbuzz)
    set_target_properties(pr_harfbuzz PROPERTIES EXPORT_NAME harfbuzz)
    target_include_directories(pr_harfbuzz SYSTEM PUBLIC $<BUILD_INTERFACE:${harfbuzz_SOURCE_DIR}/src>)
    target_compile_options(pr_harfbuzz PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-exceptions -fno-rtti>)
    pr_third_party(pr_harfbuzz)
    set_property(GLOBAL APPEND PROPERTY PR_LIBRARIES pr_harfbuzz)

    target_sources(pr_text PRIVATE backends/harfbuzz/src/text_harfbuzz.cpp)
    pr_headers(pr_text PUBLIC backends/harfbuzz/include)
    target_compile_definitions(pr_text PRIVATE PR_TEXT_HAS_HARFBUZZ)
    target_link_libraries(pr_text PRIVATE prax::harfbuzz)
endif()

if (PR_TEXT_CORETEXT)
    target_sources(pr_text PRIVATE backends/coretext/src/text_coretext.cpp)
    pr_headers(pr_text PUBLIC backends/coretext/include)
    target_compile_definitions(pr_text PRIVATE PR_TEXT_HAS_CORETEXT)
    target_link_libraries(pr_text PUBLIC "-framework CoreText" "-framework CoreFoundation" "-framework CoreGraphics")
endif()

if (PR_DEVELOP)
    pr_module_tests(text
        tests/test_text.cpp
        tests/test_text_break.cpp
        tests/test_text_c.c
        tests/test_text_estimate.cpp
        tests/test_text_grid.cpp
        tests/test_text_shaping.cpp
        tests/test_text_transform.cpp
        backends/simple/tests/test_text_simple.cpp
        backends/simple/tests/test_text_simple_c.c
    )
    target_link_libraries(text_tests PRIVATE prax::font prax::locale)
    target_compile_definitions(text_tests PRIVATE PR_FONTS_DIR="${PR_TEST_FONTS_DIR}")
endif()
