# Basis Universal's reference transcoder and encoder. The tests compare the ETC1S and UASTC decoders of lib/basis with
# them. Only a development build fetches them. They are built from their sources with their own copy of Zstandard, and
# without the SSE kernels and the OpenCL support that their own build can add.
pr_dependency_available(basis_universal)

set(basis ${basis_universal_SOURCE_DIR})
file(GLOB basis_encoder ${basis}/encoder/*.cpp ${basis}/encoder/3rdparty/*.cpp)
add_library(basis_reference STATIC ${basis}/transcoder/basisu_transcoder.cpp ${basis_encoder} ${basis}/zstd/zstd.c)
target_include_directories(basis_reference SYSTEM PUBLIC ${basis}/transcoder ${basis}/encoder)
target_compile_definitions(basis_reference PUBLIC BASISD_SUPPORT_KTX2=1 BASISD_SUPPORT_KTX2_ZSTD=1 BASISU_SUPPORT_SSE=0
                                                  BASISU_SUPPORT_OPENCL=0)
pr_third_party(basis_reference)
# The reference uses exceptions, run-time type information and the standard library. The engine's own targets build
# without them.
target_compile_options(basis_reference PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fexceptions -frtti>)
# The reference encoder converts floats that are not numbers to integers as it tries selectors, which the undefined
# behavior sanitizer stops at. The tests compare with the reference as it is released, so the check is off for it.
target_compile_options(basis_reference PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-sanitize=float-cast-overflow>)

# The tests decode textures with lib/basis, through lib/ktx, and compare the results with the reference transcoder's.
# The inputs are the KTX 2.0 textures of Khronos's sample assets and textures that the reference encoder writes.
add_executable(basis_reference_tests tests/test_basis_reference.cpp)
target_compile_definitions(basis_reference_tests PRIVATE PR_ROOT_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(basis_reference_tests PRIVATE prax::compress_select basis_reference prax::gpu prax::ktx
                                                    prax::core_cpp harness
                                                    test_main pr_warnings)
pr_add_tests(basis_reference_tests)
