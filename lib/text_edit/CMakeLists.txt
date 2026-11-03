# Text editing: an edited text, the views that select in it, and the record of its changes.
pr_library(text_edit INCLUDE include
    src/text_edit.cpp
)
target_link_libraries(pr_text_edit PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(text_edit
        tests/test_text_edit.cpp
        tests/test_text_edit_c.c
    )
endif()
