# lib/etc2's decoders against the reference unpackers of Basis Universal's encoder, which tools/suite/basis builds.
add_executable(etc2_reference_tests tests/test_etc2_reference.cpp)
target_link_libraries(etc2_reference_tests PRIVATE basis_reference prax::etc2 prax::core_cpp harness test_main pr_warnings)
pr_add_tests(etc2_reference_tests)
