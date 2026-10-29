# The references of the Lottie suites, which players other than the engine draw.

if (APPLE)
    # The Lottie references that players in the system's browser draw, which tools/suite/reftest.py drives.
    pr_harness(lottie_reference prax::app lottie_reference.cpp)
    target_link_libraries(lottie_reference PRIVATE harness_png prax::app)
endif()

if (APPLE OR WIN32)
    # rlottie, another Lottie player, which the viewer compares lib/lottie with, and the references of the rlottie
    # suite, which it draws and tools/suite/reftest.py drives.
    # A static library of the player alone. Its build predates CMake's current policies: it enables ASM, which MSVC
    # does not assemble, and installs to /usr/lib unless told otherwise, an absolute path that CMake refuses on Windows.
    # Its NEON drawing needs 32-bit ARM assembly, so an arm64 build draws without it. Its warnings are its own.
    block(SCOPE_FOR VARIABLES)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
        set(LIB_INSTALL_DIR lib)
        set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
        set(CMAKE_POLICY_DEFAULT_CMP0194 NEW)
        set(BUILD_SHARED_LIBS OFF)
        set(LOTTIE_MODULE OFF)
        set(LOTTIE_THREAD OFF)
        FetchContent_MakeAvailable(rlottie)
    endblock()
    pr_third_party(rlottie)
    # It builds without exceptions, so MSVC's standard library is told not to use them, or its headers warn (C4530)
    # despite /w.
    if (MSVC)
        target_compile_definitions(rlottie PRIVATE _HAS_EXCEPTIONS=0)
    endif()
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        target_compile_options(rlottie PRIVATE -U__ARM_NEON__)
    endif()
    # Its check for undefined symbols is meant for its shared library. The linker now warns about it in whatever links
    # the static library.
    get_target_property(PR_RLOTTIE_LINKS rlottie INTERFACE_LINK_LIBRARIES)
    list(FILTER PR_RLOTTIE_LINKS EXCLUDE REGEX "undefined")
    set_target_properties(rlottie PROPERTIES INTERFACE_LINK_LIBRARIES "${PR_RLOTTIE_LINKS}")

    pr_harness(rlottie_reference rlottie::rlottie rlottie_reference.cpp)
    target_link_libraries(rlottie_reference PRIVATE harness_png)
endif()
