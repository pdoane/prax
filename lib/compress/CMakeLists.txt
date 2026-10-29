# Compression: the interface, and the backend that the build turns on.
pr_library(compress INCLUDE include
    src/compress.cpp
    src/compress_zip.cpp
)
target_link_libraries(pr_compress PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_COMPRESS_BUILTIN)
    target_sources(pr_compress PRIVATE
        backends/builtin/src/brotli.cpp
        backends/builtin/src/brotli_tables.cpp
        backends/builtin/src/compress_builtin.cpp
        backends/builtin/src/deflate.cpp
        backends/builtin/src/inflate.cpp
    )
    pr_headers(pr_compress PUBLIC backends/builtin/include)
    target_compile_definitions(pr_compress PRIVATE PR_COMPRESS_HAS_BUILTIN)
endif()

if (PR_DEVELOP)
    pr_module_tests(compress
        tests/test_compress.cpp
        tests/test_compress_c.c
        tests/test_compress_zip.cpp
    )
    if (PR_COMPRESS_BUILTIN)
        target_sources(compress_tests PRIVATE
            backends/builtin/tests/test_brotli.cpp
            backends/builtin/tests/test_deflate.cpp
            backends/builtin/tests/test_inflate.cpp
        )
    endif()
endif()
