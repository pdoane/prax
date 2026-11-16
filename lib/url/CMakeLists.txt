# URL and URLSearchParams, as the URL Standard specifies them.
pr_library(url INCLUDE include
    src/url_data.cpp
    src/url_file.cpp
    src/url_percent.cpp
    src/url_public_suffix.cpp
    src/url.cpp
    src/url_record.cpp
    src/url_scheme.cpp
)
target_link_libraries(pr_url PUBLIC prax::core prax::charset PRIVATE prax::core_cpp prax::unicode)

if (PR_JS)
    pr_library(url_quickjs INCLUDE quickjs/include
        quickjs/src/url_quickjs.cpp
    )
    target_link_libraries(pr_url_quickjs PUBLIC prax::url prax::quickjs PRIVATE prax::webidl_quickjs_cpp
                                         prax::core_quickjs_cpp)
endif()

if (PR_DEVELOP)
    pr_module_tests(url
        tests/test_url_api_c.c
        tests/test_url_api.cpp
        tests/test_url_data.cpp
        tests/test_url_data_c.c
        tests/test_url_file.cpp
        tests/test_url_percent.cpp
        tests/test_url_percent_c.c
        tests/test_url.cpp
    )
    # The urltestdata.json harness, which tools/suite/url_parsing.py drives.
    pr_harness(url_parsing prax::url tests/url_parsing.cpp)
    if (PR_JS)
        pr_module_tests(url_quickjs
            quickjs/tests/test_url_quickjs.cpp
        )
        target_link_libraries(url_quickjs_tests PRIVATE prax::core_quickjs_cpp)
    endif()
endif()
