# Core: the platform, the foundation types, and the toolkit that every implementation is written with.
pr_library(core INCLUDE include
    src/assert.cpp
    src/base64.cpp
    src/checksum.cpp
    src/environment.cpp
    src/file.cpp
    src/float.cpp
    src/float_convert.cpp
    src/integer.cpp
    src/memory.cpp
    src/mutex.cpp
    src/pixels.cpp
    src/random.cpp
    src/status.cpp
    src/raw_string.cpp
    src/text_buffer.cpp
    src/thread.cpp
    src/time.cpp
    src/time_date.cpp
    src/utf8.cpp
    src/vec.cpp
    src/vmem.cpp
)
# Every implementation includes internal headers by their path under lib, as "<module>/src/<name>.h".
target_include_directories(pr_core PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/lib>)
# Core's sources use the header-only parts of its C++ layer, which links core in turn.
target_include_directories(pr_core PRIVATE cpp/include)
# Allocations are counted in every build except the one the benchmarks measure. The tests of code that frees memory read
# the counts.
target_compile_definitions(pr_core PRIVATE $<$<NOT:$<CONFIG:Release>>:PR_ALLOC_COUNT>)

# The platform's build configuration (docs/platform.md). Every target that builds on core sees it.
if (PR_USER_CONFIG)
    target_compile_definitions(pr_core PUBLIC "PR_USER_CONFIG=\"${PR_USER_CONFIG}\"")
endif()
if (PR_PLATFORM_NO_DEFAULT)
    target_compile_definitions(pr_core PUBLIC PR_PLATFORM_NO_DEFAULT)
endif()
if (PR_PLATFORM_MIMALLOC)
    target_compile_definitions(pr_core PUBLIC PR_PLATFORM_MIMALLOC)
endif()

# The platform's backends, which core binds to by default (docs/platform.md): the C library for memory on every target,
# or mimalloc if the build chooses it, and a family of the target for the other groups. They include core's headers
# without linking it, because core links them.
pr_library(core_libc INCLUDE backends/libc/include
    backends/libc/src/core_libc.cpp
)
target_include_directories(pr_core_libc PRIVATE ${PROJECT_SOURCE_DIR}/lib include)

# mimalloc replaces nothing, because the library reaches it only through the platform.
if (PR_PLATFORM_MIMALLOC OR PR_DEVELOP)
    set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    # A Debug build keeps mimalloc's assertions and its fill of freed memory, so a read of freed memory fails loudly.
    # It is still optimized and built without the internal checks, because every allocation passes through it,
    # including the runtime's. Every other build turns the checks off. Both are forced, because the setting is cached:
    # a directory configured once as Debug would keep the checks otherwise.
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(MI_DEBUG ON CACHE STRING "" FORCE)
    else()
        set(MI_DEBUG OFF CACHE STRING "" FORCE)
    endif()
    pr_dependency_available(mimalloc)
    pr_third_party_directory(${mimalloc_SOURCE_DIR})
    # MSVC refuses to optimize a Debug build's runtime checks, so mimalloc stays unoptimized there.
    if (CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT MSVC)
        target_compile_options(mimalloc-static PRIVATE -O2)
    endif()

    pr_library(core_mimalloc INCLUDE backends/mimalloc/include
        backends/mimalloc/src/core_mimalloc.cpp
    )
    target_include_directories(pr_core_mimalloc PRIVATE ${PROJECT_SOURCE_DIR}/lib include)
    target_link_libraries(pr_core_mimalloc PRIVATE mimalloc-static)
endif()

# The POSIX family, which serves the groups other than memory on the Unix-like targets, and the Windows family, which
# serves them on Windows. A target builds the family that it has.
if (WIN32)
    pr_library(core_windows INCLUDE backends/windows/include
        backends/windows/src/core_windows_environment.cpp
        backends/windows/src/core_windows_fail.cpp
        backends/windows/src/core_windows_file.cpp
        backends/windows/src/core_windows_random.cpp
        backends/windows/src/core_windows_thread.cpp
        backends/windows/src/core_windows_time.cpp
        backends/windows/src/core_windows_vmem.cpp
    )
    target_include_directories(pr_core_windows PRIVATE ${PROJECT_SOURCE_DIR}/lib include)
    # BCryptGenRandom, which the random bytes read.
    target_link_libraries(pr_core_windows PRIVATE bcrypt)
else()
    pr_library(core_posix INCLUDE backends/posix/include
        backends/posix/src/core_posix_environment.cpp
        backends/posix/src/core_posix_fail.cpp
        backends/posix/src/core_posix_file.cpp
        backends/posix/src/core_posix_random.cpp
        backends/posix/src/core_posix_thread.cpp
        backends/posix/src/core_posix_time.cpp
        backends/posix/src/core_posix_vmem.cpp
    )
    target_include_directories(pr_core_posix PRIVATE ${PROJECT_SOURCE_DIR}/lib include)
endif()
# The C++ conveniences of the C headers, such as PrStringView's conversions, are C++20.
target_compile_features(pr_core PUBLIC cxx_std_20)

if (NOT PR_PLATFORM_NO_DEFAULT AND PR_PLATFORM_MIMALLOC)
    target_link_libraries(pr_core PUBLIC prax::core_mimalloc)
elseif (NOT PR_PLATFORM_NO_DEFAULT)
    target_link_libraries(pr_core PUBLIC prax::core_libc)
endif()
if (NOT PR_PLATFORM_NO_DEFAULT AND WIN32)
    target_link_libraries(pr_core PUBLIC prax::core_windows)
elseif (NOT PR_PLATFORM_NO_DEFAULT)
    target_link_libraries(pr_core PUBLIC prax::core_posix)
endif()

# Core's C++ layer: the toolkit that every library's implementation is written with, and the C++ side of the platform.
pr_library(core_cpp INCLUDE cpp/include
    cpp/src/atom_table.cpp
    cpp/src/format.cpp
    cpp/src/hash_index.cpp
    cpp/src/number_list.cpp
    cpp/src/writer.cpp
)
target_link_libraries(pr_core_cpp PUBLIC prax::core)

# QuickJS, built from its sources as its own build builds them: what the QuickJS layers run JavaScript with. It is C11
# with GNU extensions, and other compilers than MSVC read a plain char as unsigned.
if (PR_JS)
    pr_dependency_available(quickjs)
    add_library(pr_quickjs STATIC
        ${quickjs_SOURCE_DIR}/cutils.c
        ${quickjs_SOURCE_DIR}/libbf.c
        ${quickjs_SOURCE_DIR}/libregexp.c
        ${quickjs_SOURCE_DIR}/libunicode.c
        ${quickjs_SOURCE_DIR}/quickjs.c
    )
    add_library(prax::quickjs ALIAS pr_quickjs)
    # Its header is a system's, so that the warnings of every compiler stay on the engine's own code.
    set_target_properties(pr_quickjs PROPERTIES EXPORT_NAME quickjs C_STANDARD 11 C_EXTENSIONS ON
                                                C_VISIBILITY_PRESET hidden SYSTEM ON)
    target_sources(pr_quickjs PUBLIC FILE_SET HEADERS BASE_DIRS ${quickjs_SOURCE_DIR}
                                     FILES ${quickjs_SOURCE_DIR}/quickjs.h ${quickjs_SOURCE_DIR}/quickjs-atom.h)
    target_compile_definitions(pr_quickjs PRIVATE _GNU_SOURCE $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>)
    target_compile_options(pr_quickjs PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/experimental:c11atomics,-funsigned-char>)
    find_package(Threads REQUIRED)
    target_link_libraries(pr_quickjs PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    find_library(PR_LIBM m)
    if (PR_LIBM)
        target_link_libraries(pr_quickjs PRIVATE m)
    endif()
    pr_third_party(pr_quickjs)
    # The big-number library takes its AVX2 path with GCC's vector operators, which MSVC does not have, so MSVC builds
    # the portable path.
    if (MSVC)
        get_target_property(PR_QUICKJS_OPTIONS pr_quickjs COMPILE_OPTIONS)
        list(FILTER PR_QUICKJS_OPTIONS EXCLUDE REGEX "AVX2")
        set_target_properties(pr_quickjs PROPERTIES COMPILE_OPTIONS "${PR_QUICKJS_OPTIONS}")
    endif()
    set_property(GLOBAL APPEND PROPERTY PR_LIBRARIES pr_quickjs)

    # Core's QuickJS C++ layer, which makes native handles into JS objects: the toolkit that the QuickJS layers are
    # written with.
    pr_library(core_quickjs_cpp INCLUDE quickjs_cpp/include
        quickjs_cpp/src/quickjs_binding.cpp
        quickjs_cpp/src/quickjs_callback.cpp
    )
    target_link_libraries(pr_core_quickjs_cpp PUBLIC prax::core prax::core_cpp prax::quickjs)
endif()

if (PR_DEVELOP)
    pr_module_tests(core
        tests/test_base64.cpp
        tests/test_byte_view.cpp
        tests/test_checksum.cpp
        tests/test_core_c.c
        tests/test_file.cpp
        tests/test_float.cpp
        tests/test_float16.cpp
        tests/test_hash.cpp
        tests/test_integer.cpp
        tests/test_pixels.cpp
        tests/test_platform.cpp
        tests/test_environment.cpp
        tests/test_random.cpp
        tests/test_raw_string.cpp
        tests/test_string_ops.cpp
        tests/test_string_view.cpp
        tests/test_thread.cpp
        tests/test_time_date.cpp
        tests/test_utf8.cpp
    )
    target_link_libraries(core_tests PRIVATE prax::core_libc prax::core_mimalloc)
    target_compile_definitions(core_tests PRIVATE PR_CORE_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}/core_tests_files")

    pr_module_tests(core_cpp
        cpp/tests/test_raw_array.cpp
        cpp/tests/test_array.cpp
        cpp/tests/test_atom_table.cpp
        cpp/tests/test_bitset.cpp
        cpp/tests/test_format.cpp
        cpp/tests/test_function.cpp
        cpp/tests/test_hash_index.cpp
        cpp/tests/test_map.cpp
        cpp/tests/test_min_max.cpp
        cpp/tests/test_name_table.cpp
        cpp/tests/test_number_list.cpp
        cpp/tests/test_print.cpp
        cpp/tests/test_search.cpp
        cpp/tests/test_sort.cpp
        cpp/tests/test_span.cpp
        cpp/tests/test_string.cpp
        cpp/tests/test_vmem.cpp
        cpp/tests/test_writer.cpp
    )

    # A build of core's memory, failure and strings configured by a header, with the builtin platform left out.
    add_library(core_configured STATIC
        src/assert.cpp
        src/memory.cpp
        src/raw_string.cpp
    )
    target_include_directories(core_configured PUBLIC ${PROJECT_SOURCE_DIR}/lib include)
    target_include_directories(core_configured PRIVATE cpp/include)
    target_compile_definitions(core_configured PUBLIC
        "PR_USER_CONFIG=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/test_platform_config.h\"")
    target_link_libraries(core_configured PRIVATE prax::core_libc prax::core_mimalloc pr_warnings)
    if (WIN32)
        target_link_libraries(core_tests PRIVATE prax::core_windows)
        target_link_libraries(core_configured PRIVATE prax::core_windows)
    else()
        target_link_libraries(core_tests PRIVATE prax::core_posix)
        target_link_libraries(core_configured PRIVATE prax::core_posix)
    endif()
    add_executable(core_config_tests tests/test_platform_config.cpp)
    target_link_libraries(core_config_tests PRIVATE core_configured test_main pr_warnings)
    # The tests' doctest header (tools/harness/test.h) prints the C++ layer's strings, which are header-only over the
    # configured core, so the layer's headers are all that this binary needs of it.
    target_include_directories(core_config_tests PRIVATE cpp/include)
    doctest_discover_tests(core_config_tests)

    if (PR_JS)
        pr_module_tests(core_quickjs_cpp
            quickjs_cpp/tests/test_quickjs_callback.cpp
        )
    endif()
endif()
