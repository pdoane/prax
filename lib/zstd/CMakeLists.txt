# Zstandard: the decoding of its streams, and the encoding of a whole input as one frame.
pr_library(zstd INCLUDE include
    src/zstd.cpp
    src/zstd_format.cpp
)
target_link_libraries(pr_zstd PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(zstd
        tests/test_zstd.cpp
        tests/test_zstd_c.c
        tests/test_zstd_encode.cpp
    )
endif()
