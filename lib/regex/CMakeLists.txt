# Regular expressions in RE2's syntax, matched in time that grows with the pattern and the text together.
pr_library(regex INCLUDE include
    src/regex.cpp
)
target_link_libraries(pr_regex PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(regex
        tests/test_regex.cpp
        tests/test_regex_c.c
    )
endif()
