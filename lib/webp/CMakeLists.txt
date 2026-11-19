# WebP: its container and extended format, its lossy and lossless bitstreams, alpha and animation, decoded to pixels.
pr_library(webp INCLUDE include
    src/vp8.cpp
    src/vp8_tables.cpp
    src/vp8l.cpp
    src/webp.cpp
)
target_link_libraries(pr_webp PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(webp
        tests/test_webp.cpp
        tests/test_webp_c.c
    )
endif()
