# Text diffs: the lines that differ between two texts, the characters that differ within the lines that changed, and
# three-way merges with their conflict markers.
pr_library(text_diff INCLUDE include
    src/text_diff.cpp
    src/text_diff_merge.cpp
    src/text_diff_sequence.cpp
)
target_link_libraries(pr_text_diff PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(text_diff
        tests/test_text_diff.cpp
        tests/test_text_diff_c.c
        tests/test_text_diff_merge.cpp
    )
endif()
