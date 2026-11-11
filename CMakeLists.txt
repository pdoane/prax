# The consumer minimum (README.md, CMake versions). shader_library.cmake and tools/package state it too.
cmake_minimum_required(VERSION 3.28)
# Where Prax is built on its own, Apple builds target Apple Silicon and the oldest macOS or iOS that Prax runs on, which
# share a version. A build that includes Prax chooses its own, and Prax checks them below. These precede project(), which
# reads them.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "The architectures that Apple builds target")
    if (NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET 26.0 CACHE STRING "The oldest macOS or iOS that the build runs on" FORCE)
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

# The platforms that Prax runs on (README.md): macOS 26 or newer on Apple Silicon, iOS, tvOS and visionOS 26 or newer,
# and Windows 10 version 1809 or newer. A build that targets an older system or another architecture fails here rather
# than in a library's sources. On Windows, the system headers then declare only what 1809 has.
if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(PR_MACOS ON)
elseif (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(PR_IOS ON)
elseif (CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    set(PR_TVOS ON)
elseif (CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    set(PR_VISIONOS ON)
endif()
# The platforms whose applications are UIKit's.
if (PR_IOS OR PR_TVOS OR PR_VISIONOS)
    set(PR_UIKIT ON)
endif()
if (APPLE)
    if (CMAKE_OSX_ARCHITECTURES)
        set(PR_OSX_ARCHITECTURES ${CMAKE_OSX_ARCHITECTURES})
    else()
        set(PR_OSX_ARCHITECTURES ${CMAKE_SYSTEM_PROCESSOR})
    endif()
    if (NOT PR_OSX_ARCHITECTURES STREQUAL "arm64")
        message(FATAL_ERROR "Prax runs on Apple Silicon only, and this build targets ${PR_OSX_ARCHITECTURES}")
    endif()
    if (CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 26.0)
        message(FATAL_ERROR "Prax runs on ${CMAKE_SYSTEM_NAME} 26 or newer, and this build targets ${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
elseif (WIN32)
    add_compile_definitions(WINVER=0x0A00 _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000006)
endif()

# What the build makes. By default it makes the libraries, and installs them where Prax is built on its own. A
# development build (./dev.sh) adds their tests and what the tests drive, with warnings as errors, and by default the
# tools that check and measure them with the comparisons (PR_TOOLS).
option(PR_DEVELOP "Build the tests, and by default the tools and the comparisons, with warnings as errors" OFF)
# The development minimum (README.md, CMake versions).
if (PR_DEVELOP AND CMAKE_VERSION VERSION_LESS 4.0)
    message(FATAL_ERROR "A development build needs CMake 4.0 or newer, and this is CMake ${CMAKE_VERSION}")
endif()
# The tools of a development build that run beside the tests: the suites' harnesses, the viewer, the benchmarks, the
# browsers' hosts and the pages' checks, with the implementations that they compare the engine with. Without them a
# development build has the tests and what the tests drive, as the builds for the web and for UIKit's platforms do,
# and fetches none of the comparisons.
include(CMakeDependentOption)
cmake_dependent_option(PR_TOOLS "Build the development tools and the comparisons" ON
    "PR_DEVELOP;NOT EMSCRIPTEN;NOT PR_UIKIT" OFF)
option(PR_EXAMPLES "Build the examples" OFF)
option(PR_APPS "Build the apps" OFF)
option(PR_INSTALL "Install the libraries, their headers and the CMake package" ${PROJECT_IS_TOP_LEVEL})

# JavaScript, over QuickJS: the JS bindings, pr_shell, and Lottie's expressions. The browser needs it.
option(PR_JS "Run JavaScript, over QuickJS" ON)
if (PR_APPS)
    set(PR_JS ON)
endif()
# Optional parts of libraries. A program that does not need a part can leave out its tables. If a part is left out, a
# request for it reports NotSupported or finds no charset. A development build tests every part, so it needs them all.
option(PR_CHARSET_LEGACY "Decode and encode the Encoding Standard's legacy single-byte and multi-byte charsets" ON)
option(PR_COMPRESS_BROTLI "Decode Brotli in lib/compress's built-in codec" ON)
if (PR_DEVELOP AND NOT (PR_CHARSET_LEGACY AND PR_COMPRESS_BROTLI))
    message(FATAL_ERROR "PR_DEVELOP tests every charset and Brotli, so it needs PR_CHARSET_LEGACY and PR_COMPRESS_BROTLI")
endif()

# The platform's build configuration (docs/platform.md).
set(PR_USER_CONFIG "" CACHE FILEPATH "A header that binds platform facilities at compile time")
option(PR_PLATFORM_MIMALLOC "Allocate with mimalloc where the build leaves memory to the default backends" OFF)
# The web runs a program on one thread unless its page has shared memory, which Prax does not ask of it.
if (EMSCRIPTEN)
    set(PR_PLATFORM_THREADS_DEFAULT OFF)
else()
    set(PR_PLATFORM_THREADS_DEFAULT ON)
endif()
option(PR_PLATFORM_THREADS "Start threads where the build leaves threads to the default backends"
    ${PR_PLATFORM_THREADS_DEFAULT})

# The font bundles whose files the build fetches into font_bundle in the build directory, for the tools, the examples
# and the apps (lib/font_bundle/font_bundle.cmake). Every bundle by default; a build that runs only the tests, such as
# CI's, can fetch fewer. The tests read the fonts they need from the test data (cmake/test_data.cmake), the bundles'
# among them. A consumer fetches its own with pr_font_bundle_fetch.
if (PR_DEVELOP OR PR_EXAMPLES OR PR_APPS)
    set(PR_FONT_BUNDLES_DEFAULT Complete NotoMono Standard14 Plex NerdFontSymbols)
else()
    set(PR_FONT_BUNDLES_DEFAULT "")
endif()
set(PR_FONT_BUNDLES "${PR_FONT_BUNDLES_DEFAULT}" CACHE STRING "The font bundles that the build fetches")

# A lean build, which fetches only what its tests read, replaces the options above that decide what it fetches
# (cmake/lean.cmake).
option(PR_LEAN "Build the development build's tests and what they drive, and fetch only what they read" OFF)
include(cmake/lean.cmake)

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

# Emscripten's programs get 8 MB of stack, as the platforms' main threads do, and a heap that grows. WebAssembly's
# 128-bit SIMD, which every browser that runs WebAssembly well has, lets loops vectorize.
if (EMSCRIPTEN)
    add_compile_options(-msimd128)
    add_link_options(-sSTACK_SIZE=8388608 -sALLOW_MEMORY_GROWTH=1)
    # Debug compiles with the optimizer's first level, and keeps its assertions. Without it, a function keeps each of its
    # temporaries in a WebAssembly local, and the engine's frame for QuickJS's interpreter grows so large that a few
    # nested calls of JavaScript exhaust the engine's own stack, which neither Node.js nor a browser lets a page raise,
    # long before QuickJS's limit stops them.
    add_compile_options($<$<CONFIG:Debug>:-O1>)
endif()

if (PR_DEVELOP)
    # Warnings are errors in every target, whatever warnings each enables. Third-party targets build with their
    # warnings off (pr_third_party).
    set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
    # The programs are in the build directory's root, where dev.sh and the runners find them.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
    # Under Emscripten, the tests run under Node.js and read and write the host's files, and doctest's failed
    # requirements throw, which WebAssembly's own exceptions carry.
    if (EMSCRIPTEN)
        add_compile_options(-fwasm-exceptions)
        add_link_options(-fwasm-exceptions -sNODERAWFS=1)
    endif()
endif()

include(GNUInstallDirs)
include(cmake/build.cmake)
include(cmake/dependencies.cmake)

# The backends that the libraries build (README.md). A library holds each of its backends that the build turns on, and
# a program that brings its own implementation of an interface turns the library's off. A backend is on by default
# where it is the platform's own API or the library's own code, and off where it builds on a third-party library. The
# tests, the tools, the examples and the apps build every backend that the platform supports.
if (CMAKE_SYSTEM_NAME MATCHES "^(Linux|Android)$")
    set(PR_LINUX ON)
endif()
if (UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    set(PR_UNIX_OTHER ON)
endif()
# The targets that run on an operating system's own interfaces, which the web lacks: its sockets, and a GPU through
# Vulkan or a system's own API.
if (NOT EMSCRIPTEN)
    set(PR_SYSTEM ON)
endif()
# The targets whose Vulkan the Vulkan backend presents through: the desktop systems.
if (PR_SYSTEM AND NOT PR_UIKIT)
    set(PR_DESKTOP_VULKAN ON)
endif()
# The POSIX targets whose applications may start programs: the desktop systems, and not the mobile ones.
if (CMAKE_SYSTEM_NAME MATCHES "^(Darwin|Linux)$")
    set(PR_POSIX_PROCESSES ON)
endif()
if (PR_DEVELOP OR PR_EXAMPLES OR PR_APPS OR PR_UNIX_OTHER)
    find_package(OpenSSL 3...<4 QUIET)
endif()
pr_backend_option(PR_APP_APPKIT "lib/app's AppKit backend" "${PR_MACOS}" "${PR_MACOS}")
pr_backend_option(PR_APP_UIKIT "lib/app's UIKit backend" "${PR_UIKIT}" "${PR_UIKIT}")
pr_backend_option(PR_APP_WIN32 "lib/app's Win32 backend" "${WIN32}" "${WIN32}")
pr_backend_option(PR_APP_WEB "lib/app's web backend" "${EMSCRIPTEN}" "${EMSCRIPTEN}")
pr_backend_option(PR_AUDIO_APPLE "lib/audio's Core Audio backend" "${APPLE}" "${APPLE}")
pr_backend_option(PR_AUDIO_WINDOWS "lib/audio's WASAPI backend" "${WIN32}" "${WIN32}")
pr_backend_option(PR_AUDIO_MEMORY "lib/audio's memory backend" ON ON)
pr_backend_option(PR_COMPRESS_BUILTIN "lib/compress's built-in backend" ON ON)
pr_backend_option(PR_DRAW_CPU "lib/draw's CPU renderer" ON ON)
pr_backend_option(PR_DRAW_CANVAS "lib/draw's Canvas 2D renderer" "${EMSCRIPTEN}" "${EMSCRIPTEN}")
pr_backend_option(PR_DRAW_TRIANGLES "lib/draw's triangles renderer" "${PR_SYSTEM}" "${PR_SYSTEM}")
pr_backend_option(PR_DRAW_FRAGMENT "lib/draw's fragment renderer" "${PR_SYSTEM}" "${PR_SYSTEM}")
pr_backend_option(PR_DRAW_SVG "lib/draw's SVG writer" ON ON)
pr_backend_option(PR_DRAW_PDF "lib/draw's PDF writer" ON ON)
pr_backend_option(PR_FETCH_INTERCEPT "lib/fetch's fetcher whose host answers its requests" ON ON)
pr_backend_option(PR_FETCH_BUILTIN "lib/fetch's built-in backend" ON ON)
pr_backend_option(PR_FONT_CORETEXT "lib/font's CoreText backend of the system's fonts" "${APPLE}" "${APPLE}")
pr_backend_option(PR_FONT_DIRECTWRITE "lib/font's DirectWrite backend of the system's fonts" "${WIN32}" "${WIN32}")
pr_backend_option(PR_FONT_FREETYPE "lib/font's FreeType hinter" OFF ON)
pr_backend_option(PR_GPU_METAL "lib/gpu's Metal backend" "${APPLE}" "${APPLE}")
pr_backend_option(PR_GPU_METAL4 "lib/gpu's Metal 4 backend, for experiments" "${PR_MACOS}" "${PR_MACOS}")
pr_backend_option(PR_GPU_D3D12 "lib/gpu's Direct3D 12 backend" "${WIN32}" "${WIN32}")
pr_backend_option(PR_GPU_VULKAN "lib/gpu's Vulkan backend" "${PR_LINUX}" "${PR_DESKTOP_VULKAN}")
pr_backend_option(PR_GPU_MEMORY "lib/gpu's memory backend" ON ON)
pr_backend_option(PR_GPU_CANVAS2D "lib/gpu's Canvas 2D backend" "${EMSCRIPTEN}" "${EMSCRIPTEN}")
pr_backend_option(PR_HTTP_BUILTIN "lib/http's built-in backend" ON ON)
pr_backend_option(PR_IMAGE_BUILTIN "lib/image's built-in backend" ON ON)
pr_backend_option(PR_MEDIA_APPLE "lib/media's AVFoundation backend" "${APPLE}" "${APPLE}")
pr_backend_option(PR_MEDIA_WINDOWS "lib/media's Media Foundation backend" "${WIN32}" "${WIN32}")
pr_backend_option(PR_MEDIA_BUILTIN "lib/media's built-in backend" ON ON)
pr_backend_option(PR_MEDIA_WEB "lib/media's web backend, over a page's video element" "${EMSCRIPTEN}" "${EMSCRIPTEN}")
pr_backend_option(PR_NET_SOCKETS "lib/net's backend over the system's sockets" "${PR_SYSTEM}" "${PR_SYSTEM}")
pr_backend_option(PR_NET_NULL "lib/net's null network, whose loop its owner runs" ON ON)
pr_backend_option(PR_PROCESS_POSIX "lib/process's POSIX backend" "${PR_POSIX_PROCESSES}" "${PR_POSIX_PROCESSES}")
pr_backend_option(PR_PROCESS_WINDOWS "lib/process's Windows backend" "${WIN32}" "${WIN32}")
pr_backend_option(PR_SYNTAX_TREESITTER "lib/syntax's tree-sitter languages" OFF ON)
pr_backend_option(PR_TASK_BUILTIN "lib/task's built-in backend" ON ON)
pr_backend_option(PR_TEXT_HARFBUZZ "lib/text's HarfBuzz shaper" OFF ON)
pr_backend_option(PR_TEXT_KB "lib/text's kb shaper" OFF ON)
pr_backend_option(PR_TEXT_CORETEXT "lib/text's CoreText shaper" "${APPLE}" "${APPLE}")
pr_backend_option(PR_TEXT_SIMPLE "lib/text's simple shaper" ON ON)
pr_backend_option(PR_TLS_APPLE "lib/tls's Network.framework backend" "${APPLE}" "${APPLE}")
pr_backend_option(PR_TLS_SCHANNEL "lib/tls's SChannel backend" "${WIN32}" "${WIN32}")
pr_backend_option(PR_TLS_OPENSSL "lib/tls's OpenSSL backend" "${PR_UNIX_OTHER}" "${OpenSSL_FOUND}")
pr_backend_option(PR_WEBSOCKET_BUILTIN "lib/websocket's built-in backend" ON ON)
pr_backend_option(PR_WEBVIEW_WEBKIT "lib/webview's WebKit backend" "${PR_MACOS}" "${PR_MACOS}")
pr_backend_option(PR_WEBVIEW_WEBVIEW2 "lib/webview's WebView2 backend" "${WIN32}" "${WIN32}")
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
    # A metallib runs on the platform that it was compiled for: macOS, or the devices or the simulator of a UIKit
    # platform.
    if (PR_MACOS)
        set(PR_METAL_SDK macosx)
    else()
        if (PR_IOS)
            set(PR_METAL_SDK iphone)
        elseif (PR_TVOS)
            set(PR_METAL_SDK appletv)
        else()
            set(PR_METAL_SDK xr)
        endif()
        if (CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
            string(APPEND PR_METAL_SDK simulator)
        else()
            string(APPEND PR_METAL_SDK os)
        endif()
    endif()
elseif (WIN32)
    set(PR_SHADER_TARGETS dxil)
endif()
if (PR_GPU_VULKAN)
    pr_dependency_available(vulkan_headers)
    set(PR_VULKAN_HEADERS ${vulkan_headers_SOURCE_DIR}/include)
    # Vulkan has two shader targets, the baseline and local read (cmake/shader_library.cmake). The build compiles those
    # that the system's drivers support. KosmicKrisp on Apple's systems and the drivers on Windows lack dynamic rendering
    # local read, so they get only the baseline. Other systems compile both.
    if (APPLE OR WIN32)
        list(APPEND PR_SHADER_TARGETS spirv)
    else()
        list(APPEND PR_SHADER_TARGETS spirv_local_read spirv)
    endif()
endif()

# Where the build's programs find the font bundles' files.
set(PR_FONT_BUNDLE_DIR "${PROJECT_BINARY_DIR}/font_bundle")
if (PR_FONT_BUNDLES)
    pr_font_bundle_fetch("${PR_FONT_BUNDLE_DIR}" ${PR_FONT_BUNDLES})
endif()

if (PR_DEVELOP)
    # The files that the tests read.
    include(cmake/test_data.cmake)
    # The runners, the tests' servers and the tools' tests are Python.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    # Its build predates CMake's current policies.
    block(SCOPE_FOR VARIABLES)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
        pr_dependency_available(doctest)
    endblock()
    pr_third_party_directory(${doctest_SOURCE_DIR})
endif()

add_subdirectory(lib)
if (PR_DEVELOP)
    add_subdirectory(tools)
endif()
# The examples run where lib/app has a backend.
if (PR_EXAMPLES AND (PR_MACOS OR PR_UIKIT OR WIN32 OR EMSCRIPTEN))
    add_subdirectory(examples)
endif()
# The apps run on the desktop and on UIKit's platforms.
if (PR_APPS AND (PR_MACOS OR PR_UIKIT OR WIN32))
    add_subdirectory(apps)
endif()
if (PR_INSTALL)
    include(cmake/install.cmake)
endif()
if (PR_DEVELOP)
    pr_write_tests()
endif()
