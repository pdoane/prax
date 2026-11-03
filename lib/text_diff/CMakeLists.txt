# Text diffs: the lines that differ between two texts, and the characters that differ within the lines that changed.
pr_library(text_diff INCLUDE include
    src/text_diff.cpp
    src/text_diff_sequence.cpp
)
target_link_libraries(pr_text_diff PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(text_diff
        tests/test_text_diff.cpp
        tests/test_text_diff_c.c
    )
endif()
