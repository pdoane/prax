# OpenEXR: scan line and tiled images, of one part or several, with their levels, channels and attributes, decoded in
# the scene-linear values that the file holds. Chunks of zlib inflate through the compression codec that an image is
# given.
pr_library(exr INCLUDE include
    src/exr.cpp
    src/exr_compress.cpp
    src/exr_dwa.cpp
    src/exr_piz.cpp
)
target_link_libraries(pr_exr PUBLIC prax::core prax::color prax::compress PRIVATE prax::core_cpp)
# DWA's decoding computes in floats as the reference implementation does, without fusing multiplies and adds, so that
# it decodes to the same halves on every machine.
if (NOT MSVC)
    set_source_files_properties(src/exr_dwa.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()

if (PR_DEVELOP)
    pr_module_tests(exr
        tests/test_exr.cpp
        tests/test_exr_c.c
    )
    target_link_libraries(exr_tests PRIVATE prax::color)
endif()
