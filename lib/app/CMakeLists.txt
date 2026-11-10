# Applications: the interface, and the backends that the build turns on.
pr_library(app INCLUDE include
    src/app.cpp
    src/app_display.cpp
)
target_link_libraries(pr_app PUBLIC prax::core prax::color prax::geom PRIVATE prax::core_cpp)

if (PR_APP_COCOA)
    target_sources(pr_app PRIVATE backends/cocoa/src/app_cocoa.mm)
    pr_headers(pr_app PUBLIC backends/cocoa/include)
    set_source_files_properties(backends/cocoa/src/app_cocoa.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_compile_definitions(pr_app PRIVATE PR_APP_HAS_COCOA)
    target_link_libraries(pr_app PUBLIC "-framework AppKit" "-framework QuartzCore" "-framework UserNotifications")
endif()

# The Win32 backend: a Win32 window and its message loop.
if (PR_APP_WIN32)
    target_sources(pr_app PRIVATE
        backends/win32/src/app_win32.cpp
    )
    pr_headers(pr_app PUBLIC backends/win32/include)
    target_compile_definitions(pr_app PRIVATE PR_APP_HAS_WIN32)
    target_link_libraries(pr_app PUBLIC user32 gdi32 shell32 imm32 dwmapi dxgi advapi32 ole32)
endif()

# The web backend: a web page's canvas and its event loop, through Emscripten's HTML5 API.
if (PR_APP_WEB)
    target_sources(pr_app PRIVATE backends/web/src/app_web.cpp)
    pr_headers(pr_app PUBLIC backends/web/include)
    target_compile_definitions(pr_app PRIVATE PR_APP_HAS_WEB)
    pr_js_library(pr_app backends/web/src/app_web.js)
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
