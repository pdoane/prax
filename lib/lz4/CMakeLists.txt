# LZ4: the decoding of its blocks.
pr_library(lz4 INCLUDE include
    src/lz4.cpp
)
target_link_libraries(pr_lz4 PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(lz4
        tests/test_lz4.cpp
        tests/test_lz4_c.c
    )
endif()
