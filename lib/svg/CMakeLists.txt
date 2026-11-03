# SVG: its elements, rendering them into draw lists, and the glyph decoder that draws a font's SVG documents.
pr_library(svg INCLUDE include
    src/svg.cpp
    src/svg_cascade.cpp
    src/svg_document.cpp
    src/svg_draw.cpp
    src/svg_filter.cpp
    src/svg_font.cpp
    src/svg_glyph.cpp
    src/svg_hints.cpp
    src/svg_image.cpp
    src/svg_length.cpp
    src/svg_parse.cpp
    src/svg_query.cpp
    src/svg_render.cpp
    src/svg_style.cpp
    src/svg_text.cpp
    src/svg_tree.cpp
)
target_link_libraries(pr_svg PUBLIC prax::dom prax::css  prax::draw prax::text prax::font prax::layout
                                    prax::color prax::compress prax::image
                             PRIVATE prax::core_cpp prax::path prax::url prax::geom prax::unicode prax::locale)

if (PR_JS)
    pr_library(svg_quickjs INCLUDE quickjs/include
        quickjs/src/svg_quickjs.cpp
    )
    target_link_libraries(pr_svg_quickjs PUBLIC prax::svg prax::quickjs prax::css_quickjs prax::dom_quickjs PRIVATE
                                         prax::core_quickjs_cpp prax::webidl_quickjs_cpp)
endif()

if (PR_DEVELOP)
    pr_module_tests(svg
        tests/test_svg.cpp
        tests/test_svg_embedded.cpp
        tests/test_svg_painting.cpp
        tests/test_svg_parse.cpp
        tests/test_svg_filters.cpp
        tests/test_svg_glyph.cpp
        tests/test_svg_glyph_c.c
        tests/test_svg_interaction.cpp
        tests/test_svg_masking.cpp
        tests/test_svg_pservers.cpp
        tests/test_svg_shapes.cpp
        tests/test_svg_structure.cpp
        tests/test_svg_text.cpp
    )
    # An SVG document stands alone, as the XML parser creates it. Text is shaped with kb, and what a test reads as
    # pixels is rendered on the CPU.
    target_link_libraries(svg_tests PRIVATE prax::css prax::dom prax::draw prax::geom prax::path prax::text prax::compress
                                            prax::image)
    target_compile_definitions(svg_tests PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")
    if (PR_JS)
        add_test(NAME svg_js_dom COMMAND pr_shell --html ${CMAKE_CURRENT_SOURCE_DIR}/tests/svg_dom.html)
        set_tests_properties(svg_js_dom PROPERTIES PASS_REGULAR_EXPRESSION "svg dom tests passed" TIMEOUT 10)
    endif()
endif()
