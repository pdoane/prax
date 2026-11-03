# The windowed view of a text buffer: rows for the lines near the viewport, a height index, and scrolling.
pr_library(text_view INCLUDE include
    src/text_view.cpp
)
target_link_libraries(pr_text_view PUBLIC prax::color prax::core prax::geom prax::layout prax::text_edit
    PRIVATE prax::core_cpp prax::text)

if (PR_DEVELOP)
    pr_module_tests(text_view
        tests/test_text_view.cpp
        tests/test_text_view_c.c
    )
    target_link_libraries(text_view_tests PRIVATE prax::font prax::layout prax::text)
    target_compile_definitions(text_view_tests PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")
endif()
