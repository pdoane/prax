# BC: the block-compressed formats of Direct3D, BC1 to BC7, and their decoding to pixels in software.
pr_library(bc INCLUDE include
    src/bc.cpp
)
target_link_libraries(pr_bc PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(bc
        tests/test_bc.cpp
        tests/test_bc_c.c
    )
endif()
