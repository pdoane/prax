# The harnesses of our own suites (tools/suite/prax).

# Decoders given their inputs damaged (suites/prax-decoders).
pr_harness(decoder_check prax::zstd decoder_check.cpp)
target_include_directories(decoder_check PRIVATE ${PROJECT_SOURCE_DIR}/lib)
target_link_libraries(decoder_check PRIVATE prax::brotli prax::lzfse prax::webp prax::font prax::core_cpp)

# The path library's fills and combinations over shapes (suites/prax-paths).
pr_harness(path_check prax::path path_check.cpp)
target_link_libraries(path_check PRIVATE prax::core_cpp)
