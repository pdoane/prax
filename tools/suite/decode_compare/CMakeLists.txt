# The harness of the suites of image corpora (tools/suite/decode_compare.py): lib/image's decoders over files, beside
# the PNGs of their pictures.
pr_harness(decode_compare harness_codecs decode_compare.cpp)
target_link_libraries(decode_compare PRIVATE prax::core_cpp)
