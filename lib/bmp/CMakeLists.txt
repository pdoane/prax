# BMP: the device-independent bitmaps of BMP files and of icons, of every version of their information header.
pr_library(bmp INCLUDE include
    src/bmp.cpp
)
target_link_libraries(pr_bmp PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(bmp
        tests/test_bmp.cpp
        tests/test_bmp_c.c
    )
endif()
