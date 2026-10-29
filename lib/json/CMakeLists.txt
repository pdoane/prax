# JSON: text read into a document of values, and text written from values.
pr_library(json INCLUDE include
    src/json.cpp
)
target_link_libraries(pr_json PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(json
        tests/test_json.cpp
        tests/test_json_c.c
    )
endif()
