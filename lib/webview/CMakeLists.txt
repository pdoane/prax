# Web views: the interface, and the backends that the build turns on.
pr_library(webview INCLUDE include
    src/webview.cpp
)
target_link_libraries(pr_webview PUBLIC prax::core prax::app prax::geom PRIVATE prax::core_cpp)

if (PR_WEBVIEW_WEBKIT)
    target_sources(pr_webview PRIVATE backends/webkit/src/webview_webkit.mm)
    pr_headers(pr_webview PUBLIC backends/webkit/include)
    set_source_files_properties(backends/webkit/src/webview_webkit.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_compile_definitions(pr_webview PRIVATE PR_WEBVIEW_HAS_WEBKIT)
    target_link_libraries(pr_webview PUBLIC "-framework AppKit" "-framework QuartzCore" "-framework WebKit")
endif()

# The WebView2 backend, over the WebView2 SDK, the NuGet package that its headers and loader come from. The runtime is
# the system's.
if (PR_WEBVIEW_WEBVIEW2)
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

    target_sources(pr_webview PRIVATE backends/webview2/src/webview_webview2.cpp)
    pr_headers(pr_webview PUBLIC backends/webview2/include)
    target_compile_definitions(pr_webview PRIVATE PR_WEBVIEW_HAS_WEBVIEW2)
    target_include_directories(pr_webview SYSTEM PRIVATE ${webview2_SOURCE_DIR}/build/native/include)
    target_link_libraries(pr_webview PUBLIC user32 ole32 windowscodecs version
                                            $<BUILD_INTERFACE:${PR_WEBVIEW2_LOADER}>
                                            $<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/WebView2LoaderStatic.lib>
                                     PRIVATE prax::json)
endif()

if (PR_DEVELOP)
    pr_module_tests(webview
        tests/test_webview.cpp
        tests/test_webview_c.c
    )
    # A backend's tests run its engine under an application's event loop.
    if (PR_WEBVIEW_WEBKIT)
        target_sources(webview_tests PRIVATE backends/webkit/tests/test_webview_webkit.mm)
        set_source_files_properties(backends/webkit/tests/test_webview_webkit.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    endif()
    if (PR_WEBVIEW_WEBVIEW2)
        target_sources(webview_tests PRIVATE backends/webview2/tests/test_webview_webview2.cpp)
    endif()
    target_link_libraries(webview_tests PRIVATE prax::app)
endif()
