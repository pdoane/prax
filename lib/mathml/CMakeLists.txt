# MathML: its elements, their attributes and the operator dictionary, their hints, and what they give their boxes.
pr_library(mathml INCLUDE include
    src/mathml.cpp
    src/mathml_attribute.cpp
    src/mathml_boxes.cpp
    src/mathml_dictionary.cpp
    src/mathml_element.cpp
    src/mathml_hints.cpp
    src/mathml_operator.cpp
)
target_link_libraries(pr_mathml PUBLIC prax::dom PRIVATE prax::core_cpp prax::css prax::layout prax::text prax::unicode)

if (PR_JS)
    pr_library(mathml_quickjs INCLUDE quickjs/include
        quickjs/src/mathml_quickjs.cpp
    )
    target_link_libraries(pr_mathml_quickjs PUBLIC prax::mathml prax::quickjs prax::css_quickjs prax::dom_quickjs
                                            PRIVATE prax::core_quickjs_cpp prax::webidl_quickjs_cpp)
endif()

if (PR_DEVELOP)
    pr_module_tests(mathml
        tests/test_mathml.cpp
        tests/test_mathml_c.c
        tests/test_mathml_layout.cpp
        tests/test_mathml_style.cpp
    )
    target_link_libraries(mathml_tests PRIVATE prax::css prax::dom prax::html prax::unicode prax::font prax::text
                                               prax::layout)
    target_compile_definitions(mathml_tests PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts"
                                                    PR_FONT_BUNDLE_DIR="${PR_FONT_BUNDLE_DIR}")
endif()
