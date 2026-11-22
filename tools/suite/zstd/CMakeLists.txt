# Zstandard's reference library, which lib/zstd's decoder and encoder are compared with. It is fetched for the
# development build alone and built from its sources, without the assembly that its own build adds on some machines.
pr_dependency_available(zstd)

file(GLOB zstd_sources ${zstd_SOURCE_DIR}/lib/common/*.c ${zstd_SOURCE_DIR}/lib/compress/*.c
                       ${zstd_SOURCE_DIR}/lib/decompress/*.c)
add_library(zstd_reference STATIC ${zstd_sources})
target_include_directories(zstd_reference SYSTEM PUBLIC ${zstd_SOURCE_DIR}/lib)
target_compile_definitions(zstd_reference PRIVATE ZSTD_DISABLE_ASM)
pr_third_party(zstd_reference)

# The harness of the zstd suite (tools/suite/zstd.py): lib/zstd beside the reference library, both ways.
pr_harness(zstd_compare prax::zstd zstd_compare.cpp)
target_link_libraries(zstd_compare PRIVATE zstd_reference prax::core_cpp)
