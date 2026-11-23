# JPEG: Huffman-coded, 8-bit, sequential and progressive images, with their JFIF and Exif metadata.
pr_library(jpeg INCLUDE include
    src/jpeg.cpp
)
target_link_libraries(pr_jpeg PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(jpeg
        tests/test_jpeg.cpp
        tests/test_jpeg_c.c
    )
    # A sequential and a progressive JPEG that the tests append in pieces.
    pr_resources(jpeg_tests
        ${PROJECT_SOURCE_DIR}/suites/prax/html-04-the-elements-of-html/support/flag-exif-144dpi.jpg@jpeg/baseline.jpg
        ${PROJECT_SOURCE_DIR}/suites/prax/workloads/support/cat.jpg@jpeg/progressive.jpg)
endif()
