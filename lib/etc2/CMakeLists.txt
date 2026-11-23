# ETC2 and EAC: the block-compressed formats of OpenGL ES 3 and Vulkan, and their decoding to pixels in software.
pr_library(etc2 INCLUDE include
    src/etc2.cpp
)
target_link_libraries(pr_etc2 PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(etc2
        tests/test_etc2.cpp
        tests/test_etc2_c.c
    )
endif()
