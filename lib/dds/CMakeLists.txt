# DDS, the container of Direct3D textures. The library reads a texture as the texels that it stores, blocks of a format
# that a GPU samples (lib/bc) or pixels, and decodes them to pixels where it can. It also makes DDS files texture
# sources.
pr_library(dds INCLUDE include
    src/dds.cpp
    src/dds_texture_source.cpp
)
target_link_libraries(pr_dds PUBLIC prax::core prax::gpu prax::texture PRIVATE prax::bc prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(dds
        tests/test_dds.cpp
        tests/test_dds_c.c
        tests/test_dds_texture_source.cpp
    )
    target_link_libraries(dds_tests PRIVATE prax::gpu prax::texture)
endif()
