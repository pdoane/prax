# CSS: style sheets, their values, the CSSOM and the cascade.
pr_library(css INCLUDE include
    src/css.cpp
    src/css_animate.cpp
    src/css_animation.cpp
    src/css_at_rule.cpp
    src/css_boxes.cpp
    src/css_cascade.cpp
    src/css_color.cpp
    src/css_color_value.cpp
    src/css_computed.cpp
    src/css_counter.cpp
    src/css_counter_pass.cpp
    src/css_counter_style.cpp
    src/css_filter.cpp
    src/css_font_face.cpp
    src/css_gradient.cpp
    src/css_grammar.cpp
    src/css_grid.cpp
    src/css_highlight.cpp
    src/css_hints.cpp
    src/css_inline_style.cpp
    src/css_intersection_observer.cpp
    src/css_keyword.cpp
    src/css_match.cpp
    src/css_math.cpp
    src/css_media.cpp
    src/css_object.cpp
    src/css_owner.cpp
    src/css_parse.cpp
    src/css_parse_terminal.cpp
    src/css_parser.cpp
    src/css_path.cpp
    src/css_presentation.cpp
    src/css_presentation_hooks.cpp
    src/css_presentation_svg.cpp
    src/css_property.cpp
    src/css_pseudo.cpp
    src/css_resize_observer.cpp
    src/css_root.cpp
    src/css_rule.cpp
    src/css_selector.cpp
    src/css_selector_match.cpp
    src/css_serialize.cpp
    src/css_shape.cpp
    src/css_shorthand.cpp
    src/css_style.cpp
    src/css_style_group.cpp
    src/css_supports.cpp
    src/css_syntax.cpp
    src/css_syntax_definition.cpp
    src/css_syntax_rule.cpp
    src/css_terminal.cpp
    src/css_transform.cpp
    src/css_unit.cpp
    src/css_value.cpp
    src/css_value_def.cpp
    src/css_view.cpp
)
target_link_libraries(pr_css PUBLIC prax::dom prax::core prax::url prax::layout prax::draw prax::font prax::geom prax::task
                                    prax::app prax::color
                             PRIVATE prax::core_cpp prax::charset prax::unicode prax::text prax::compress prax::path
                                     prax::locale)

if (PR_JS)
    pr_library(css_quickjs INCLUDE quickjs/include
        quickjs/src/css_quickjs.cpp
        quickjs/src/css_typed_om_quickjs.cpp
    )
    target_link_libraries(pr_css_quickjs PUBLIC prax::css prax::quickjs prax::dom_quickjs PRIVATE prax::color
                                         prax::core_quickjs_cpp prax::webidl_quickjs_cpp)
endif()

if (PR_DEVELOP)
    pr_module_tests(css
        tests/test_css_c_api.c
        tests/test_css_filter.cpp
        tests/test_css_geometry.cpp
        tests/test_css_grammar.cpp
        tests/test_css_math.cpp
        tests/test_css_media.cpp
        tests/test_css_object.cpp
        tests/test_css_owner.cpp
        tests/test_css_rule.cpp
        tests/test_css_selector.cpp
        tests/test_css_selector_match.cpp
        tests/test_css_serialize.cpp
        tests/test_css_shape.cpp
        tests/test_css_shorthand.cpp
        tests/test_css_stream.cpp
        tests/test_css_supports.cpp
        tests/test_css_terminal.cpp
        tests/test_css_token.cpp
        tests/test_css_unit.cpp
        tests/test_css_color.cpp
        tests/test_css_counter.cpp
        tests/test_css_value.cpp
        tests/test_cssom_value.cpp
        tests/test_css.cpp
    )
    target_link_libraries(css_tests PRIVATE prax::dom prax::path prax::url prax::color)

    # The css-parsing-tests harness, which tools/suite/css_parsing.py drives.
    pr_harness(css_parsing prax::css tests/css_parsing.cpp)

    # The engine's own benchmark, which ./dev.sh bench css-parse runs beside the shell's.
    pr_harness(css_bench prax::css tests/css_bench.cpp)

    # The Typed OM benchmark's cases through the C API, which ./dev.sh bench typed-om runs beside the shell's.
    pr_harness(typed_bench prax::css tests/typed_bench.cpp)

    if (PR_JS)
        pr_module_tests(css_quickjs
            quickjs/tests/test_typed_value_quickjs.cpp
        )
        target_link_libraries(css_quickjs_tests PRIVATE prax::dom_quickjs prax::core_quickjs_cpp prax::webidl_quickjs_cpp)
    endif()
endif()
