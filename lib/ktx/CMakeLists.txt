# KTX 2.0, the container format for GPU textures. The library reads a texture as pixels or as the blocks that a GPU
# samples, and transcodes the Basis Universal formats to those blocks. It inflates a supercompressed level with the
# compression codec that the caller gives the texture.
pr_library(ktx INCLUDE include
    src/ktx.cpp
    src/ktx_texture_source.cpp
)
target_link_libraries(pr_ktx PUBLIC prax::core prax::compress prax::gpu prax::texture PRIVATE prax::basis prax::bc prax::etc2
                                                                                     prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(ktx
        tests/test_ktx.cpp
        tests/test_ktx_c.c
        tests/test_ktx_texture_source.cpp
    )
    target_link_libraries(ktx_tests PRIVATE prax::compress_select prax::bc prax::etc2 prax::compress prax::gpu prax::texture)
endif()
