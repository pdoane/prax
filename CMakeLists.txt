# The consumer minimum (README.md, CMake versions). shader_library.cmake and tools/package state it too.
cmake_minimum_required(VERSION 3.28)
# Where Prax is built on its own, macOS builds target Apple Silicon and the oldest macOS that Prax runs on. A build that
# includes Prax chooses its own, and Prax checks them below. These precede project(), which reads them.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "The architectures that macOS builds target")
    if (NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET 26.0 CACHE STRING "The oldest macOS that the build runs on" FORCE)
    endif()
endif()
project(prax
    VERSION 0.1.0
    DESCRIPTION "C libraries for GUI applications and games"
    LANGUAGES C CXX)
# The Apple backends are Objective-C++.
if (APPLE)
    enable_language(OBJCXX)
endif()

# The platforms that Prax runs on (README.md): macOS 26 or newer on Apple Silicon, and Windows 10 version 1809 or newer.
# A build that targets an older system or another architecture fails here rather than in a library's sources. On
# Windows, the system headers then declare only what 1809 has.
if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if (CMAKE_OSX_ARCHITECTURES)
        set(PR_OSX_ARCHITECTURES ${CMAKE_OSX_ARCHITECTURES})
    else()
        set(PR_OSX_ARCHITECTURES ${CMAKE_SYSTEM_PROCESSOR})
    endif()
    if (NOT PR_OSX_ARCHITECTURES STREQUAL "arm64")
        message(FATAL_ERROR "Prax runs on Apple Silicon only, and this build targets ${PR_OSX_ARCHITECTURES}")
    endif()
    if (CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 26.0)
        message(FATAL_ERROR "Prax runs on macOS 26 or newer, and this build targets ${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
elseif (WIN32)
    add_compile_definitions(WINVER=0x0A00 _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000006)
endif()

# What the build makes. By default it makes the libraries, and installs them where Prax is built on its own. A
# development build (./dev.sh) adds their tests, the tools that check and measure them, and the comparisons, with
# warnings as errors.
option(PR_DEVELOP "Build the tests, the tools and the comparisons, with warnings as errors" OFF)
# The development minimum (README.md, CMake versions).
if (PR_DEVELOP AND CMAKE_VERSION VERSION_LESS 4.0)
    message(FATAL_ERROR "A development build needs CMake 4.0 or newer, and this is CMake ${CMAKE_VERSION}")
endif()
option(PR_EXAMPLES "Build the examples" OFF)
option(PR_INSTALL "Install the libraries, their headers and the CMake package" ${PROJECT_IS_TOP_LEVEL})

# JavaScript, over QuickJS: the JS bindings, pr_shell, and Lottie's expressions. The examples need it.
option(PR_JS "Run JavaScript, over QuickJS" ON)
if (PR_EXAMPLES)
    set(PR_JS ON)
endif()

# The platform's build configuration (docs/platform.md).
set(PR_USER_CONFIG "" CACHE FILEPATH "A header that binds platform facilities at compile time")
option(PR_PLATFORM_NO_DEFAULT "Leave the default platform backends out, so that PR_USER_CONFIG binds every group" OFF)
option(PR_PLATFORM_MIMALLOC "Allocate with mimalloc where the build leaves memory to the default backends" OFF)

# The font bundles whose files the build fetches, into font_bundle in the build directory, for the tests, the tools and
# the examples (lib/font_bundle/font_bundle.cmake). A consumer fetches its own with pr_font_bundle_fetch.
if (PR_DEVELOP OR PR_EXAMPLES)
    set(PR_FONT_BUNDLES_DEFAULT "Noto;Standard14")
endif()
set(PR_FONT_BUNDLES "${PR_FONT_BUNDLES_DEFAULT}" CACHE STRING "The font bundles whose files the build fetches")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The public headers are a C API, so they are compiled as C as well.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
# The library uses no C++ modules, so no source is scanned for them.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
# Each library once on a link line, which the linker would otherwise warn about when a library comes again after one
# that depends on it.
set(CMAKE_LINK_LIBRARIES_STRATEGY REORDER_FREELY)

# An x86-64 build targets x86-64-v3, whose F16C converts a half float in an instruction, as arm64 does. Every target
# builds for it, the third-party ones included.
if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    add_compile_options($<IF:$<C_COMPILER_ID:MSVC>,/arch:AVX2,-march=x86-64-v3>)
endif()

# MSVC reads sources as UTF-8, preprocesses as the standard says, and lets an object hold more sections than its
# default, which the generated tables pass.
if (MSVC)
    add_compile_options(/utf-8 /Zc:preprocessor /bigobj)
    # The C library's standard functions, such as fopen, and its POSIX ones, such as dup, are not deprecated in favor of
    # Microsoft's own.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    # A program's main thread gets 8 MB of stack on macOS and Linux, and 1 MB on Windows unless the program reserves
    # more. The build's programs reserve 8 MB. Recursion that the library bounds, such as a CSS value's nesting to
    # kPrCssValueMaxDepth, then fits in a Debug build's frames as it does elsewhere. QuickJS's own limit of 1 MB also
    # fits inside the stack instead of matching all of it.
    add_link_options(/STACK:8388608)
endif()

if (PR_DEVELOP)
    # Warnings are errors in every target, whatever warnings each enables. Third-party targets build with their
    # warnings off (pr_third_party).
    set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
    # The programs are in the build directory's root, where dev.sh and the runners find them.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
endif()

include(GNUInstallDirs)
include(cmake/build.cmake)
include(cmake/dependencies.cmake)

# The backends that the libraries build (README.md). A library holds each of its backends that the build turns on, and
# a program that brings its own implementation of an interface turns the library's off. A backend is on by default
# where it is the platform's own API or the library's own code, and off where it builds on a third-party library. The
# tests, the tools and the examples build every backend that the platform supports.
if (CMAKE_SYSTEM_NAME MATCHES "^(Linux|Android)$")
    set(PR_LINUX ON)
endif()
if (UNIX AND NOT APPLE)
    set(PR_UNIX_OTHER ON)
endif()
# The POSIX targets whose applications may start programs: the desktop systems, and not the mobile ones.
if (CMAKE_SYSTEM_NAME MATCHES "^(Darwin|Linux)$")
    set(PR_POSIX_PROCESSES ON)
endif()
if (PR_DEVELOP OR PR_EXAMPLES OR PR_UNIX_OTHER)
    find_package(OpenSSL 3...<4 QUIET)
endif()
pr_backend_option(PR_APP_COCOA "lib/app's Cocoa backend" "${APPLE}" "${APPLE}")
pr_backend_option(PR_APP_WIN32 "lib/app's Win32 backend" "${WIN32}" "${WIN32}")
pr_backend_option(PR_GPU_METAL "lib/gpu's Metal backend, on Metal 3" "${APPLE}" "${APPLE}")
pr_backend_option(PR_GPU_METAL4 "lib/gpu's Metal 4 backend, for experiments" "${APPLE}" "${APPLE}")
pr_backend_option(PR_GPU_D3D12 "lib/gpu's Direct3D 12 backend" "${WIN32}" "${WIN32}")
pr_backend_option(PR_GPU_VULKAN "lib/gpu's Vulkan backend" "${PR_LINUX}" ON)
pr_backend_option(PR_NET_SOCKETS "lib/net's backend over the system's sockets" ON ON)
pr_backend_option(PR_NET_NULL "lib/net's null network, whose loop its owner runs" ON ON)
pr_backend_option(PR_PROCESS_POSIX "lib/process's POSIX backend" "${PR_POSIX_PROCESSES}" "${PR_POSIX_PROCESSES}")
pr_backend_option(PR_PROCESS_WINDOWS "lib/process's Windows backend" "${WIN32}" "${WIN32}")
pr_backend_option(PR_FETCH_INTERCEPT "lib/fetch's fetcher whose host answers its requests" ON ON)
pr_backend_option(PR_TLS_APPLE "lib/tls's Network.framework backend" "${APPLE}" "${APPLE}")
pr_backend_option(PR_TLS_SCHANNEL "lib/tls's SChannel backend" "${WIN32}" "${WIN32}")
pr_backend_option(PR_TLS_OPENSSL "lib/tls's OpenSSL backend" "${PR_UNIX_OTHER}" "${OpenSSL_FOUND}")
pr_backend_option(PR_HTTP_BUILTIN "lib/http's built-in backend" ON ON)
pr_backend_option(PR_FETCH_BUILTIN "lib/fetch's built-in backend" ON ON)
pr_backend_option(PR_WEBSOCKET_BUILTIN "lib/websocket's built-in backend" ON ON)
pr_backend_option(PR_TASK_BUILTIN "lib/task's built-in backend" ON ON)
pr_backend_option(PR_COMPRESS_BUILTIN "lib/compress's built-in backend" ON ON)
pr_backend_option(PR_IMAGE_BUILTIN "lib/image's built-in backend" ON ON)
pr_backend_option(PR_AUDIO_APPLE "lib/audio's Core Audio backend" "${APPLE}" "${APPLE}")
pr_backend_option(PR_AUDIO_WINDOWS "lib/audio's WASAPI backend" "${WIN32}" "${WIN32}")
pr_backend_option(PR_AUDIO_MEMORY "lib/audio's memory backend" ON ON)
pr_backend_option(PR_MEDIA_APPLE "lib/media's AVFoundation backend" "${APPLE}" "${APPLE}")
pr_backend_option(PR_MEDIA_WINDOWS "lib/media's Media Foundation backend" "${WIN32}" "${WIN32}")
pr_backend_option(PR_MEDIA_BUILTIN "lib/media's built-in backend" ON ON)
pr_backend_option(PR_FONT_CORETEXT "lib/font's CoreText backend of the system's fonts" "${APPLE}" "${APPLE}")
pr_backend_option(PR_FONT_DIRECTWRITE "lib/font's DirectWrite backend of the system's fonts" "${WIN32}" "${WIN32}")
pr_backend_option(PR_FONT_FREETYPE "lib/font's FreeType hinter" OFF ON)
pr_backend_option(PR_TEXT_HARFBUZZ "lib/text's HarfBuzz shaper" OFF ON)
pr_backend_option(PR_TEXT_KB "lib/text's kb shaper" OFF ON)
pr_backend_option(PR_TEXT_CORETEXT "lib/text's CoreText shaper" "${APPLE}" "${APPLE}")
pr_backend_option(PR_TEXT_SIMPLE "lib/text's simple shaper" ON ON)
pr_backend_option(PR_SYNTAX_TREESITTER "lib/syntax's tree-sitter languages" OFF ON)
pr_backend_option(PR_DRAW_CPU "lib/draw's CPU renderer" ON ON)
pr_backend_option(PR_DRAW_TRIANGLES "lib/draw's triangles renderer" ON ON)
pr_backend_option(PR_DRAW_FRAGMENT "lib/draw's fragment renderer" ON ON)
include(lib/font_bundle/font_bundle.cmake)

# The compiler of the shaders, and the shader libraries that it compiles for each backend.
if (NOT PR_SLANGC)
    pr_dependency_available(slang)
    set(PR_SLANGC ${slang_SOURCE_DIR}/bin/slangc)
    if (CMAKE_HOST_WIN32)
        string(APPEND PR_SLANGC .exe)
    endif()
endif()
if (APPLE)
    set(PR_SHADER_TARGETS metal)
elseif (WIN32)
    set(PR_SHADER_TARGETS dxil)
endif()
if (PR_GPU_VULKAN)
    pr_dependency_available(vulkan_headers)
    set(PR_VULKAN_HEADERS ${vulkan_headers_SOURCE_DIR}/include)
    list(APPEND PR_SHADER_TARGETS spirv)
endif()

# Where the build's programs find the font bundles' files.
set(PR_FONT_BUNDLE_DIR "${PROJECT_BINARY_DIR}/font_bundle")
if (PR_FONT_BUNDLES)
    pr_font_bundle_fetch("${PR_FONT_BUNDLE_DIR}" ${PR_FONT_BUNDLES})
endif()

if (PR_DEVELOP)
    # The runners, the tests' servers and the tools' tests are Python.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    include(CTest)
    # Its build predates CMake's current policies.
    block(SCOPE_FOR VARIABLES)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
        pr_dependency_available(doctest)
    endblock()
    pr_third_party_directory(${doctest_SOURCE_DIR})
    include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
endif()

add_subdirectory(lib)
if (PR_DEVELOP)
    add_subdirectory(tools)
endif()
if (PR_EXAMPLES AND (APPLE OR WIN32))
    add_subdirectory(examples)
endif()
if (PR_INSTALL)
    include(cmake/install.cmake)
endif()
