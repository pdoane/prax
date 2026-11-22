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
target_link_libraries(pr_app PUBLIC prax::core prax::color prax::geom prax::surface PRIVATE prax::core_cpp)

# What the backends share: the delivery of a window's callbacks, the events and answers that wait for a callback, the
# displays that they have reported, the reads of the clipboard and of drops that wait for their answer, the content of
# drags, and the regions of a custom title bar.
pr_backend_shared(app shared SOURCES
    backends/shared/src/app_shared_data.cpp
    backends/shared/src/app_shared_delivery.cpp
    backends/shared/src/app_shared_displays.cpp
    backends/shared/src/app_shared_drag.cpp
    backends/shared/src/app_shared_events.cpp
    backends/shared/src/app_shared_title_bar.cpp
)

# The AppKit backend: AppKit's windows and its run loop, on macOS.
set(appkit_sources
    backends/appkit/src/app_appkit.mm
    backends/appkit/src/app_appkit_data.mm
    backends/appkit/src/app_appkit_display.mm
    backends/appkit/src/app_appkit_input.mm
    backends/appkit/src/app_appkit_menu.mm
    backends/appkit/src/app_appkit_shell.mm
    backends/appkit/src/app_appkit_window.mm
)
pr_backend(app appkit SHARED shared INCLUDE backends/appkit/include SOURCES ${appkit_sources})
if (PR_APP_APPKIT)
    set_source_files_properties(${appkit_sources} PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_link_libraries(pr_app PUBLIC "-framework AppKit" "-framework Carbon" "-framework IOKit"
                                 "-framework QuartzCore" "-framework UniformTypeIdentifiers"
                                 "-framework UserNotifications")
endif()

# The UIKit backend: UIKit's scenes and windows, on iOS and iPadOS, tvOS and visionOS.
set(uikit_sources
    backends/uikit/src/app_uikit.mm
    backends/uikit/src/uikit_data.mm
    backends/uikit/src/uikit_device.mm
    backends/uikit/src/uikit_view.mm
)
pr_backend(app uikit SHARED shared INCLUDE backends/uikit/include SOURCES ${uikit_sources})
if (PR_APP_UIKIT)
    set_source_files_properties(${uikit_sources} PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_link_libraries(pr_app PUBLIC "-framework UIKit" "-framework QuartzCore" "-framework UniformTypeIdentifiers"
                                 "-framework Foundation" "-framework CoreGraphics" "-framework UserNotifications")
    # tvOS has no motion sensors.
    if (NOT PR_TVOS)
        target_link_libraries(pr_app PUBLIC "-framework CoreMotion")
    endif()
    # visionOS's immersive space is a scene that SwiftUI hosts, and draws through CompositorServices.
    if (PR_VISIONOS)
        target_sources(pr_app PRIVATE backends/uikit/src/uikit_immersive.mm backends/uikit/src/uikit_immersive.swift)
        set_source_files_properties(backends/uikit/src/uikit_immersive.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
        target_link_libraries(pr_app PUBLIC "-framework SwiftUI" "-framework CompositorServices")
        target_link_directories(pr_app PUBLIC ${PR_SWIFT_LINK_DIRECTORIES})
    endif()
endif()

# The Win32 backend: Win32 windows and their message loop.
pr_backend(app win32 SHARED shared INCLUDE backends/win32/include SOURCES
    backends/win32/src/app_win32.cpp
    backends/win32/src/app_win32_data.cpp
    backends/win32/src/app_win32_display.cpp
    backends/win32/src/app_win32_input.cpp
    backends/win32/src/app_win32_menu.cpp
    backends/win32/src/app_win32_shell.cpp
    backends/win32/src/app_win32_window.cpp
)
if (PR_APP_WIN32)
    target_link_libraries(pr_app PUBLIC user32 gdi32 shell32 shcore imm32 dwmapi dxgi advapi32 ole32 uuid wtsapi32)
endif()

# The web backend: a web page's canvas and its event loop, through Emscripten's HTML5 API.
pr_backend(app web SHARED shared INCLUDE backends/web/include SOURCES backends/web/src/app_web.cpp)
if (PR_APP_WEB)
    pr_js_library(pr_app backends/web/src/app_web.js)
endif()

# The selector for application backends. Programs and composition libraries link it.
pr_select(app INCLUDE select/include SOURCES select/src/app_select.cpp)

if (PR_DEVELOP)
    pr_module_tests(app
        tests/test_app.cpp
        tests/test_app_c.c
        tests/test_app_popup.cpp
    )
    target_link_libraries(app_tests PRIVATE prax::surface)
    if (PR_APP_APPKIT OR PR_APP_UIKIT OR PR_APP_WIN32 OR PR_APP_WEB)
        target_sources(app_tests PRIVATE
            backends/shared/tests/test_app_shared_data.cpp
            backends/shared/tests/test_app_shared_delivery.cpp
            backends/shared/tests/test_app_shared_displays.cpp
            backends/shared/tests/test_app_shared_events.cpp
            backends/shared/tests/test_app_shared_title_bar.cpp
        )
    endif()
endif()
