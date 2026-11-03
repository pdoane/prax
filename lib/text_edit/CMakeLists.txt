# Text editing: an edited text, the views that select in it, and the record of its changes.
pr_library(text_edit INCLUDE include
    src/text_edit.cpp
    src/text_edit_command.cpp
    src/text_edit_layout.cpp
    src/text_edit_motion.cpp
)
target_link_libraries(pr_text_edit PUBLIC prax::app prax::core prax::layout prax::text
    PRIVATE prax::core_cpp prax::locale prax::unicode)

if (PR_DEVELOP)
    pr_module_tests(text_edit
        tests/test_text_edit.cpp
        tests/test_text_edit_c.c
        tests/test_text_edit_motion.cpp
    )
    target_link_libraries(text_edit_tests PRIVATE prax::font prax::layout prax::text)
    target_compile_definitions(text_edit_tests PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")
endif()
