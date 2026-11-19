# Compression: the interface, its sets of codecs, and the codecs that adapt the compression format libraries, one
# backend for each. A program carries only the codecs that it creates, and so only their format libraries.
pr_library(compress INCLUDE include
    src/compress.cpp
    src/compress_frame.cpp
)
target_link_libraries(pr_compress PUBLIC prax::core PRIVATE prax::core_cpp)

# Prax's own codecs, one for each bare format, which the build includes together.
pr_backend(compress builtin INCLUDE backends/deflate/include SOURCES
    backends/deflate/src/compress_deflate.cpp
    backends/brotli/src/compress_brotli.cpp
    backends/lzfse/src/compress_lzfse.cpp
    backends/lz4/src/compress_lz4.cpp
    backends/zstd/src/compress_zstd.cpp
)
if (PR_COMPRESS_BUILTIN)
    pr_headers(pr_compress PUBLIC backends/brotli/include)
    pr_headers(pr_compress PUBLIC backends/lzfse/include)
    pr_headers(pr_compress PUBLIC backends/lz4/include)
    pr_headers(pr_compress PUBLIC backends/zstd/include)
    target_link_libraries(pr_compress PRIVATE prax::deflate prax::brotli prax::lzfse prax::lz4 prax::zstd)
endif()

# The codec of Apple's Compression library, which a program chooses over Prax's own for a format.
pr_backend(compress apple INCLUDE backends/apple/include SOURCES backends/apple/src/compress_apple.cpp)
if (PR_COMPRESS_APPLE)
    target_link_libraries(pr_compress PRIVATE compression)
endif()

# The selector: the choice of the backend of each bare format, and a set of every format.
pr_select(compress INCLUDE select/include SOURCES select/src/compress_select.cpp)

if (PR_DEVELOP)
    pr_module_tests(compress
        tests/test_compress.cpp
        tests/test_compress_frame.cpp
        tests/test_compress_c.c
    )
    target_link_libraries(compress_tests PRIVATE prax::compress_select)
endif()
