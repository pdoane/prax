# Applications: the interface, and the backends that the build turns on.
pr_library(app INCLUDE include
    src/app_application.cpp
    src/app_data.cpp
    src/app_device.cpp
    src/app_display.cpp
    src/app_event.cpp
    src/app_input.cpp
    src/app_menu.cpp
    src/app_popup.cpp
    src/app_shell.cpp
    src/app_user_preferences.cpp
    src/app_window.cpp
)
target_link_libraries(pr_app PUBLIC prax::core prax::color prax::geom PRIVATE prax::core_cpp)

# What the backends share: the delivery of a window's callbacks, the reads of the clipboard and of drops that wait for
# their answer, the content of drags, and the regions of a custom title bar.
if (PR_APP_COCOA OR PR_APP_WIN32 OR PR_APP_WEB)
    target_sources(pr_app PRIVATE
        backends/shared/src/app_shared_data.cpp
        backends/shared/src/app_shared_delivery.cpp
        backends/shared/src/app_shared_drag.cpp
        backends/shared/src/app_shared_title_bar.cpp
    )
endif()

if (PR_APP_COCOA)
    target_sources(pr_app PRIVATE backends/cocoa/src/app_cocoa.mm)
    pr_headers(pr_app PUBLIC backends/cocoa/include)
    set_source_files_properties(backends/cocoa/src/app_cocoa.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_compile_definitions(pr_app PRIVATE PR_APP_HAS_COCOA)
    target_link_libraries(pr_app PUBLIC "-framework AppKit" "-framework Carbon" "-framework IOKit" "-framework QuartzCore"
                                 "-framework UniformTypeIdentifiers" "-framework UserNotifications")
endif()

# The Win32 backend: Win32 windows and their message loop.
if (PR_APP_WIN32)
    target_sources(pr_app PRIVATE
        backends/win32/src/app_win32.cpp
    )
    pr_headers(pr_app PUBLIC backends/win32/include)
    target_compile_definitions(pr_app PRIVATE PR_APP_HAS_WIN32)
    target_link_libraries(pr_app PUBLIC user32 gdi32 shell32 shcore imm32 dwmapi dxgi advapi32 ole32 uuid wtsapi32)
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
        tests/test_app_popup.cpp
    )
    if (PR_APP_COCOA OR PR_APP_WIN32 OR PR_APP_WEB)
        target_sources(app_tests PRIVATE
            backends/shared/tests/test_app_shared_data.cpp
            backends/shared/tests/test_app_shared_delivery.cpp
            backends/shared/tests/test_app_shared_title_bar.cpp
        )
    endif()
    if (PR_APP_WIN32)
        target_sources(app_tests PRIVATE backends/win32/tests/test_app_win32.cpp)
    endif()
    if (PR_APP_COCOA)
        target_sources(app_tests PRIVATE backends/cocoa/tests/test_app_cocoa.mm)
        set_source_files_properties(backends/cocoa/tests/test_app_cocoa.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    endif()
endif()
