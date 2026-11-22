# Textures: texture sources, which produce a texture's bits on demand, and the textures that sources realize on a GPU
# device. The library has the sources that convert the pixels of another source and make its mip levels, the format sets
# that open encoded bytes as sources, the catalog that names sources by path, the upload of a source to a GPU device,
# and the cache of the textures that sources realize on one device. The library depends on no file format. Each format
# has a library of its own.
pr_library(texture INCLUDE include
    src/texture.cpp
    src/texture_cache.cpp
    src/texture_catalog.cpp
    src/texture_source.cpp
)
target_link_libraries(pr_texture PUBLIC prax::core prax::color prax::gpu prax::task PRIVATE prax::core_cpp prax::mime)

# The selector. It creates a set of every texture format. Each format has one implementation, in its own library, so the
# selector has no backend to choose.
pr_library(texture_select INCLUDE select/include
    select/src/texture_select.cpp
)
target_link_libraries(pr_texture_select PUBLIC prax::compress prax::image prax::texture PRIVATE prax::ktx)

if (PR_DEVELOP)
    pr_module_tests(texture
        tests/test_texture.cpp
        tests/test_texture_cache.cpp
        tests/test_texture_cache_detail.cpp
        tests/test_texture_catalog.cpp
        tests/test_texture_compute.cpp
        tests/test_texture_cache_asks.cpp
        tests/test_texture_c.c
    )
    target_link_libraries(texture_tests PRIVATE prax::color prax::gpu prax::gpu_select prax::task)
    pr_slang(texture_tests tests/test_texture_compute.slang VARIABLE kTestTextureComputeShaders)
endif()
