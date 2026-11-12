# Unicode: the character properties, and the algorithms that text needs.
pr_library(unicode INCLUDE include
    src/unicode.cpp
    src/unicode_bidi.cpp
    src/unicode_bidi_data.cpp
    src/unicode_data.cpp
    src/unicode_grapheme.cpp
    src/unicode_idna.cpp
    src/unicode_idna_data.cpp
    src/unicode_line_break.cpp
    src/unicode_line_break_data.cpp
    src/unicode_normalization.cpp
    src/unicode_normalization_data.cpp
)
target_link_libraries(pr_unicode PUBLIC prax::core prax::locale PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(unicode
        tests/test_unicode.cpp
        tests/test_unicode_bidi.cpp
        tests/test_unicode_c.c
        tests/test_unicode_grapheme.cpp
        tests/test_unicode_idna.cpp
        tests/test_unicode_line_break.cpp
        tests/test_unicode_normalization.cpp
    )
    target_link_libraries(unicode_tests PRIVATE prax::locale)
    # The conformance files of the Unicode Character Database and UTS #46, which tools/suite/unicode.py drives.
    pr_harness(unicode_conformance prax::unicode tests/unicode_conformance.cpp)
endif()
