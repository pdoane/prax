# MIME types: their essences, their parameters, and the groups that mimesniff places them in.
pr_library(mime INCLUDE include
    src/mime.cpp
)
target_link_libraries(pr_mime PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(mime
        tests/test_mime.cpp
        tests/test_mime_c.c
    )
endif()
