# ICO: choosing an image from an icon's directory, and the pixels of its bitmap images. PNG images stay encoded, for
# their caller to decode. lib/bmp decodes the bitmaps.
pr_library(ico INCLUDE include
    src/ico.cpp
)
target_link_libraries(pr_ico PUBLIC prax::core PRIVATE prax::bmp prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(ico
        tests/test_ico.cpp
        tests/test_ico_c.c
    )
endif()
