# Applications: the interface, and the backends that the build turns on.
pr_library(app INCLUDE include
    src/app.cpp
    src/app_display.cpp
)
target_link_libraries(pr_app PUBLIC prax::core prax::color prax::geom PRIVATE prax::core_cpp)

if (PR_APP_COCOA)
    target_sources(pr_app PRIVATE
        backends/cocoa/src/app_cocoa.mm
        backends/cocoa/src/cocoa_browser.mm
    )
    pr_headers(pr_app PUBLIC backends/cocoa/include)
    set_source_files_properties(backends/cocoa/src/app_cocoa.mm backends/cocoa/src/cocoa_browser.mm
                                PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_compile_definitions(pr_app PRIVATE PR_APP_HAS_COCOA)
    target_link_libraries(pr_app PUBLIC "-framework AppKit" "-framework QuartzCore" "-framework WebKit")
endif()

# The Win32 backend: a Win32 window and its message loop, and the system's browser through WebView2.
if (PR_APP_WIN32)
    # The WebView2 SDK, the NuGet package that the browser's headers and loader come from. The runtime is the system's.
    pr_dependency(webview2
        URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2903.40
        URL_HASH SHA256=ef128016dd1e51c59178c827ed5b8aa3322c57afa8675d930f8109505542ad74
        DOWNLOAD_NAME webview2.zip
        SOURCE_SUBDIR none
    )
    pr_dependency_available(webview2)
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
        set(PR_WEBVIEW2_ARCH arm64)
    else()
        set(PR_WEBVIEW2_ARCH x64)
    endif()
    # The loader is installed beside the libraries, which link it by its path.
    set(PR_WEBVIEW2_LOADER ${webview2_SOURCE_DIR}/build/native/${PR_WEBVIEW2_ARCH}/WebView2LoaderStatic.lib)
    if (PR_INSTALL)
        install(FILES ${PR_WEBVIEW2_LOADER} DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()

    target_sources(pr_app PRIVATE
        backends/win32/src/app_win32.cpp
        backends/win32/src/win32_browser.cpp
        backends/win32/src/win32_text.cpp
    )
    pr_headers(pr_app PUBLIC backends/win32/include)
    target_compile_definitions(pr_app PRIVATE PR_APP_HAS_WIN32)
    target_include_directories(pr_app SYSTEM PRIVATE ${webview2_SOURCE_DIR}/build/native/include)
    target_link_libraries(pr_app PUBLIC user32 gdi32 shell32 imm32 dwmapi dxgi advapi32 ole32 windowscodecs version
                                        $<BUILD_INTERFACE:${PR_WEBVIEW2_LOADER}>
                                        $<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/WebView2LoaderStatic.lib>
                                 PRIVATE prax::json)
endif()

if (PR_DEVELOP)
    pr_module_tests(app
        tests/test_app.cpp
        tests/test_app_c.c
    )
    if (PR_APP_WIN32)
        target_sources(app_tests PRIVATE backends/win32/tests/test_app_win32.cpp)
    endif()
    if (PR_APP_COCOA)
        target_sources(app_tests PRIVATE backends/cocoa/tests/test_app_cocoa.mm)
        set_source_files_properties(backends/cocoa/tests/test_app_cocoa.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    endif()
endif()
