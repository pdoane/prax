# The browsers that the engine is compared with.

if (APPLE)
    # The system's WebKit in lib/app's browser, which ./dev.sh bench browser compares the engine with and the probes
    # and stills capture.
    pr_harness(webkit_host prax::app webkit_host.cpp)
    target_link_libraries(webkit_host PRIVATE harness_png prax::app)
endif()
