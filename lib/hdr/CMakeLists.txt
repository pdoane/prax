# Radiance HDR: the pictures of the Radiance lighting system, decoded to floats of linear light.
pr_library(hdr INCLUDE include
    src/hdr.cpp
)
target_link_libraries(pr_hdr PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(hdr
        tests/test_hdr.cpp
        tests/test_hdr_c.c
    )
endif()
