# Media: the interface, and the backends that the build turns on.
pr_library(media INCLUDE include
    src/media.cpp
)
target_link_libraries(pr_media PUBLIC prax::core PRIVATE prax::core_cpp prax::mime)

if (PR_MEDIA_BUILTIN)
    target_sources(pr_media PRIVATE backends/builtin/src/media_builtin.cpp)
    pr_headers(pr_media PUBLIC backends/builtin/include)
    target_compile_definitions(pr_media PRIVATE PR_MEDIA_HAS_BUILTIN)
endif()

if (PR_MEDIA_APPLE)
    target_sources(pr_media PRIVATE backends/apple/src/media_apple.mm)
    set_source_files_properties(backends/apple/src/media_apple.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    pr_headers(pr_media PUBLIC backends/apple/include)
    target_compile_definitions(pr_media PRIVATE PR_MEDIA_HAS_APPLE)
    target_link_libraries(pr_media PUBLIC "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo"
        "-framework CoreGraphics" "-framework Foundation" "-framework AudioToolbox")
endif()

if (PR_MEDIA_WINDOWS)
    target_sources(pr_media PRIVATE backends/windows/src/media_windows.cpp)
    pr_headers(pr_media PUBLIC backends/windows/include)
    target_compile_definitions(pr_media PRIVATE PR_MEDIA_HAS_WINDOWS)
    target_link_libraries(pr_media PUBLIC mfplat mfreadwrite mfuuid ole32 shlwapi)
endif()

if (PR_DEVELOP)
    pr_module_tests(media
        tests/test_media.cpp
        tests/test_media_c.c
    )
    if (PR_MEDIA_BUILTIN)
        target_sources(media_tests PRIVATE backends/builtin/tests/test_media_builtin.cpp)
    endif()
    if (PR_MEDIA_APPLE)
        target_sources(media_tests PRIVATE backends/apple/tests/test_media_apple.mm)
        set_source_files_properties(backends/apple/tests/test_media_apple.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    endif()
    if (PR_MEDIA_WINDOWS)
        target_sources(media_tests PRIVATE backends/windows/tests/test_media_windows.cpp)
    endif()
endif()
