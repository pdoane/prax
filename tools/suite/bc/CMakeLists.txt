# lib/bc's decoders against the reference unpackers of Basis Universal's encoder, which tools/suite/basis builds.
add_executable(bc_reference_tests tests/test_bc_reference.cpp)
target_link_libraries(bc_reference_tests PRIVATE basis_reference prax::bc prax::core_cpp harness test_main pr_warnings)
pr_add_tests(bc_reference_tests)
