# The 2D rendering context of a canvas, and the paths, gradients and patterns that it draws.
pr_library(canvas INCLUDE include
    src/canvas.cpp
    src/canvas_path.cpp
    src/canvas_text.cpp
)
target_link_libraries(pr_canvas PUBLIC prax::css prax::dom prax::draw prax::font prax::geom prax::gpu prax::layout prax::path
                                       prax::text prax::color prax::locale
                                PRIVATE prax::core_cpp)

if (PR_JS)
    pr_library(canvas_quickjs INCLUDE quickjs/include
        quickjs/src/canvas_quickjs.cpp
    )
    target_link_libraries(pr_canvas_quickjs PUBLIC prax::canvas prax::quickjs PRIVATE prax::dom_quickjs prax::html
                                            prax::webidl_quickjs_cpp prax::core_quickjs_cpp)
endif()

if (PR_DEVELOP)
    pr_module_tests(canvas
        tests/test_canvas.cpp
        tests/test_canvas_c.c
        tests/test_canvas_path.cpp
    )
    # What a test reads as pixels is rendered on the CPU, or on the platform's GPU for a bitmap on a device.
    target_link_libraries(canvas_tests PRIVATE prax::draw prax::gpu)
endif()
