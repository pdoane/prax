# OpenEXR's core library, whose decoders lib/exr's decoder is compared with and whose encoders write the files
# of each compression. It is fetched for the development build alone and built from its sources, with libdeflate's
# compressor and decompressor, as OpenEXR's own build compiles them into the library when it fetches libdeflate.
pr_dependency_available(openexr libdeflate)
find_package(Threads REQUIRED)

# The headers that OpenEXR's build configures: its version and namespaces, its threads, and Imath's, which the core
# library reads only to use Imath's half conversions where Imath is new enough, and here uses its own.
set(OPENEXR_VERSION_MAJOR 3)
set(OPENEXR_VERSION_MINOR 3)
set(OPENEXR_VERSION_PATCH 15)
set(OPENEXR_VERSION 3.3.15)
set(OPENEXR_LIB_VERSION 3.3.15)
set(OPENEXR_PACKAGE_NAME "OpenEXR 3.3.15")
set(OPENEXR_VERSION_RELEASE_TYPE "")
set(OPENEXR_NAMESPACE_CUSTOM 0)
set(OPENEXR_INTERNAL_IMF_NAMESPACE Imf_3_3)
set(OPENEXR_IMF_NAMESPACE Imf)
set(ILMTHREAD_THREADING_ENABLED 1)
set(ILMTHREAD_HAVE_POSIX_SEMAPHORES 0)
set(ILMTHREAD_NAMESPACE_CUSTOM 0)
set(ILMTHREAD_INTERNAL_NAMESPACE IlmThread_3_3)
set(ILMTHREAD_NAMESPACE IlmThread)
set(openexr_config ${CMAKE_CURRENT_BINARY_DIR}/openexr_config)
configure_file(${openexr_SOURCE_DIR}/cmake/OpenEXRConfig.h.in ${openexr_config}/OpenEXRConfig.h)
configure_file(${openexr_SOURCE_DIR}/cmake/OpenEXRConfigInternal.h.in ${openexr_config}/OpenEXRConfigInternal.h)
configure_file(${openexr_SOURCE_DIR}/cmake/IlmThreadConfig.h.in ${openexr_config}/IlmThreadConfig.h)
file(CONFIGURE OUTPUT ${openexr_config}/ImathConfig.h CONTENT "#pragma once\n")

set(openexr_core ${openexr_SOURCE_DIR}/src/lib/OpenEXRCore)
file(GLOB openexr_sources ${openexr_core}/*.c)
add_library(openexr_reference STATIC ${openexr_sources}
    ${libdeflate_SOURCE_DIR}/lib/arm/cpu_features.c ${libdeflate_SOURCE_DIR}/lib/x86/cpu_features.c
    ${libdeflate_SOURCE_DIR}/lib/utils.c ${libdeflate_SOURCE_DIR}/lib/deflate_compress.c
    ${libdeflate_SOURCE_DIR}/lib/deflate_decompress.c ${libdeflate_SOURCE_DIR}/lib/adler32.c
    ${libdeflate_SOURCE_DIR}/lib/zlib_compress.c ${libdeflate_SOURCE_DIR}/lib/zlib_decompress.c)
target_include_directories(openexr_reference SYSTEM PUBLIC ${openexr_SOURCE_DIR}/src/lib ${openexr_core}
    ${openexr_config} PRIVATE ${libdeflate_SOURCE_DIR})
target_link_libraries(openexr_reference PRIVATE Threads::Threads)
set_target_properties(openexr_reference PROPERTIES C_STANDARD 11)
# Its DWA decoding computes in floats, whose results depend on whether the compiler fuses multiplies and adds. It is
# built without fusing, as lib/exr's DWA decoder is.
if (NOT MSVC)
    target_compile_options(openexr_reference PRIVATE -ffp-contract=off)
endif()
pr_third_party(openexr_reference)

# What the comparisons share: decoding with each decoder, and the files that OpenEXR's encoders write.
add_library(exr_reference STATIC exr_reference.cpp)
target_include_directories(exr_reference PUBLIC ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/lib)
target_link_libraries(exr_reference PUBLIC prax::core_cpp PRIVATE harness_codecs openexr_reference prax::exr
                                    pr_warnings)

# The harness of the exr suite (tools/suite/exr.py) and of the exr benchmark, over files.
pr_harness(exr_compare exr_reference exr_compare.cpp)
target_link_libraries(exr_compare PRIVATE prax::core_cpp)
