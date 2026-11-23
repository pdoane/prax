# Targa (TGA): color-mapped, true-color and black and white images, stored as they are or run-length encoded.
pr_library(tga INCLUDE include
    src/tga.cpp
)
target_link_libraries(pr_tga PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(tga
        tests/test_tga.cpp
        tests/test_tga_c.c
    )
endif()
