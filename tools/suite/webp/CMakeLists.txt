# libwebp's decoders and encoders, which lib/webp's decoder is compared with. They are fetched for the development
# build alone and built from their sources, as libwebp's own build compiles them, without threads.
pr_dependency_available(libwebp)

file(GLOB libwebp_sources ${libwebp_SOURCE_DIR}/src/dec/*.c ${libwebp_SOURCE_DIR}/src/demux/*.c
                          ${libwebp_SOURCE_DIR}/src/dsp/*.c ${libwebp_SOURCE_DIR}/src/enc/*.c
                          ${libwebp_SOURCE_DIR}/src/mux/*.c ${libwebp_SOURCE_DIR}/src/utils/*.c
                          ${libwebp_SOURCE_DIR}/sharpyuv/*.c)
add_library(libwebp_reference STATIC ${libwebp_sources})
target_include_directories(libwebp_reference SYSTEM PUBLIC ${libwebp_SOURCE_DIR}/src PRIVATE ${libwebp_SOURCE_DIR})
pr_third_party(libwebp_reference)

# What the comparisons share: decoding with each decoder, and the images that libwebp's encoders write.
add_library(webp_reference STATIC webp_reference.cpp)
target_include_directories(webp_reference PUBLIC ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/lib)
target_link_libraries(webp_reference PUBLIC prax::core_cpp PRIVATE libwebp_reference prax::webp
                                     pr_warnings)

# lib/webp's decoder beside libwebp's over images of each feature, which the unit tests run.
add_executable(webp_reference_tests tests/test_webp_reference.cpp)
target_link_libraries(webp_reference_tests PRIVATE harness_codecs webp_reference prax::image prax::core_cpp test_main
                                                   pr_warnings)
pr_add_tests(webp_reference_tests)

# The harness of the webp suite (tools/suite/webp.py), over files.
pr_harness(webp_compare webp_reference webp_compare.cpp)
target_link_libraries(webp_compare PRIVATE prax::core_cpp)
