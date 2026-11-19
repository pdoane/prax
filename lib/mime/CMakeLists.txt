# MIME types: their essences, their parameters, the groups that mimesniff places them in, and the formats that a
# resource's first bytes identify.
pr_library(mime INCLUDE include
    src/mime.cpp
    src/mime_sniff.cpp
)
target_link_libraries(pr_mime PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(mime
        tests/test_mime.cpp
        tests/test_mime_c.c
        tests/test_mime_sniff.cpp
    )
endif()
