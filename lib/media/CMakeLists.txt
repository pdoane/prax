# Media: the interface, and the backends that the build turns on.
pr_library(media INCLUDE include
    src/media.cpp
    src/media_audio.cpp
    src/media_segment.cpp
    src/media_source.cpp
    src/media_source_decoder.cpp
)
target_link_libraries(pr_media PUBLIC prax::core prax::gpu PRIVATE prax::core_cpp prax::mime)

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
        "-framework CoreGraphics" "-framework Foundation" "-framework AudioToolbox" "-framework VideoToolbox")
endif()

# The web backend plays a resource in a page's video element, and gives its frames as the page's images, which a Canvas
# 2D device imports. It reads a resource's metadata with the built-in backend.
if (PR_MEDIA_WEB)
    target_sources(pr_media PRIVATE backends/web/src/media_web.cpp)
    pr_headers(pr_media PUBLIC backends/web/include)
    target_compile_definitions(pr_media PRIVATE PR_MEDIA_HAS_WEB)
    pr_js_library(pr_media backends/web/src/media_web.js)
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
        tests/test_media_segment.cpp
        tests/test_media_source.cpp
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
