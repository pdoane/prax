# QOI, the Quite OK Image format: 8-bit RGB and RGBA pixels in a simple lossless coding.
pr_library(qoi INCLUDE include
    src/qoi.cpp
)
target_link_libraries(pr_qoi PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(qoi
        tests/test_qoi.cpp
        tests/test_qoi_c.c
    )
endif()
