# Charsets: the encodings of the Encoding Standard, decoded to UTF-8 and encoded from it.
pr_library(charset INCLUDE include
    src/charset.cpp
    src/charset_labels.cpp
)
target_link_libraries(pr_charset PUBLIC prax::core PRIVATE prax::core_cpp prax::mime)
if (PR_CHARSET_LEGACY)
    target_sources(pr_charset PRIVATE src/charset_tables.cpp)
    target_compile_definitions(pr_charset PRIVATE PR_CHARSET_HAS_LEGACY)
endif()

if (PR_JS)
    pr_library(charset_quickjs INCLUDE quickjs/include
        quickjs/src/charset_quickjs.cpp
    )
    target_link_libraries(pr_charset_quickjs PUBLIC prax::charset prax::quickjs PRIVATE prax::webidl_quickjs_cpp
                                             prax::core_quickjs_cpp)
endif()

if (PR_DEVELOP)
    pr_module_tests(charset
        tests/test_charset.cpp
        tests/test_charset_c.c
    )
    # The engine's own decoding benchmark, which ./dev.sh bench text-decode runs beside a browser's.
    pr_harness(charset_bench prax::charset tests/charset_bench.cpp)
    if (PR_JS)
        pr_module_tests(charset_quickjs
            quickjs/tests/test_charset_quickjs.cpp
        )
        target_link_libraries(charset_quickjs_tests PRIVATE prax::core_quickjs_cpp)
    endif()
endif()
