# The browsers that the engine is compared with.

if (PR_WEBVIEW_WEBKIT)
    # The system's WebKit in lib/webview's WebKit backend, which ./dev.sh bench browser compares the engine with and the
    # probes and stills capture.
    pr_harness(webkit_host prax::app webkit_host.cpp)
    target_link_libraries(webkit_host PRIVATE harness_png prax::app prax::webview)
endif()
