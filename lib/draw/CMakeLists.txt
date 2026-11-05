# Drawing: draw lists, and the backends that the build turns on, which render them or write them as documents. A backend
# is built on the public API of draw and path.
pr_library(draw INCLUDE include
    src/draw.cpp
    src/draw_commands.cpp
    src/draw_display.cpp
    src/draw_filter.cpp
    src/draw_glyph.cpp
    src/identity.cpp
    src/draw_image.cpp
    src/draw_list.cpp
    src/draw_lower.cpp
    src/draw_projection.cpp
    src/draw_renderer.cpp
    src/draw_structure.cpp
    src/draw_transform.cpp
    src/draw_writer.cpp
)
target_link_libraries(pr_draw PUBLIC prax::path prax::font prax::image prax::geom prax::gpu prax::color prax::compress prax::task
                              PRIVATE prax::core_cpp)
# The display shader, which maps a frame of lib/draw's colors to a display (pr/draw_display.h).
pr_slang(pr_draw src/draw_display.slang VARIABLE kDrawDisplayShaders)

if (PR_DRAW_CPU)
    target_sources(pr_draw PRIVATE
        backends/cpu/src/draw_cpu.cpp
        backends/cpu/src/cpu_filter.cpp
        backends/cpu/src/cpu_pixels.cpp
    )
    pr_headers(pr_draw PUBLIC backends/cpu/include)
    target_compile_definitions(pr_draw PRIVATE PR_DRAW_HAS_CPU)
endif()

# The Canvas 2D renderer, which draws through a web page's canvases, and so builds only for the web.
if (PR_DRAW_CANVAS AND PR_GPU_CANVAS2D)
    target_sources(pr_draw PRIVATE backends/canvas/src/draw_canvas.cpp)
    pr_headers(pr_draw PUBLIC backends/canvas/include)
    target_compile_definitions(pr_draw PRIVATE PR_DRAW_HAS_CANVAS)
    pr_js_library(pr_draw backends/canvas/src/draw_canvas.js)
endif()

# What the backends share: the frames of the renderers that draw on the CPU, which the comparisons that draw on the CPU
# also use, and the atlas that the GPU renderers keep glyphs and paths in.
target_sources(pr_draw PRIVATE backends/shared/src/draw_shared_memory.cpp)
if (PR_DRAW_TRIANGLES OR PR_DRAW_FRAGMENT)
    target_sources(pr_draw PRIVATE backends/shared/src/draw_shared_atlas.cpp)
    pr_headers(pr_draw PUBLIC backends/shared/include)
endif()

# What the writers share: the walk that draws natively, lowers, and rasterizes the rest with the CPU renderer.
if (PR_DRAW_SVG OR PR_DRAW_PDF)
    if (NOT PR_DRAW_CPU)
        message(FATAL_ERROR "lib/draw's writers rasterize with its CPU renderer: PR_DRAW_SVG and PR_DRAW_PDF need PR_DRAW_CPU")
    endif()
    target_sources(pr_draw PRIVATE backends/shared/src/draw_shared_writer.cpp)
endif()

if (PR_DRAW_PDF)
    target_sources(pr_draw PRIVATE backends/pdf/src/draw_pdf.cpp)
    pr_headers(pr_draw PUBLIC backends/pdf/include)
    target_compile_definitions(pr_draw PRIVATE PR_DRAW_HAS_PDF)
endif()

if (PR_DRAW_SVG)
    target_sources(pr_draw PRIVATE backends/svg/src/draw_svg.cpp)
    pr_headers(pr_draw PUBLIC backends/svg/include)
    target_compile_definitions(pr_draw PRIVATE PR_DRAW_HAS_SVG)
endif()

if (PR_DRAW_FRAGMENT)
    target_sources(pr_draw PRIVATE
        backends/fragment/src/draw_fragment.cpp
        backends/fragment/src/draw_fragment_device.cpp
    )
    pr_headers(pr_draw PUBLIC backends/fragment/include)
    target_compile_definitions(pr_draw PRIVATE PR_DRAW_HAS_FRAGMENT)
    # The fragment renderer's shaders share the frame's constants and records with its C++ (draw_fragment_constants.h
    # and draw_fragment_records.h).
    pr_slang(pr_draw backends/fragment/src/draw_fragment.slang VARIABLE kDrawFragmentShaders
        INCLUDE ${PROJECT_SOURCE_DIR}/lib)
endif()

if (PR_DRAW_TRIANGLES)
    target_sources(pr_draw PRIVATE
        backends/triangles/src/draw_triangles.cpp
        backends/triangles/src/draw_triangles_device.cpp
    )
    pr_headers(pr_draw PUBLIC backends/triangles/include)
    target_compile_definitions(pr_draw PRIVATE PR_DRAW_HAS_TRIANGLES)
    pr_slang(pr_draw backends/triangles/src/draw_triangles.slang VARIABLE kDrawTrianglesShaders)
endif()

if (PR_DEVELOP)
    # The native tools compare lib/draw with the comparisons, so the web's build has no comparisons.
    if (NOT EMSCRIPTEN)
        add_subdirectory(comparisons)
    endif()

    pr_module_tests(draw
        tests/test_compare.cpp
        tests/test_draw_c.c
        tests/test_draw_list.cpp
        tests/test_draw_lower.cpp
        tests/test_draw_projection.cpp
        tests/test_draw_text.cpp
        tests/test_draw_writer.cpp
    )
    target_compile_definitions(draw_tests PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")
    target_link_libraries(draw_tests PRIVATE draw_compare prax::path prax::color prax::image prax::compress
                                             prax::font prax::gpu prax::task)
    if (PR_DRAW_CPU)
        target_sources(draw_tests PRIVATE
            backends/cpu/tests/test_draw_cpu.cpp
            backends/cpu/tests/test_draw_cpu_c.c
            backends/cpu/tests/test_draw_cpu_glyph_bitmap.cpp
            backends/cpu/tests/test_draw_cpu_text.cpp
        )
    endif()
    if (PR_DRAW_CANVAS)
        target_sources(draw_tests PRIVATE backends/canvas/tests/test_draw_canvas.cpp)
        pr_page_tests(draw backends/canvas/tests/test_draw_canvas_page.cpp
            PRELOAD "${PROJECT_SOURCE_DIR}/third_party/fonts/NotoSans.ttf@/fonts/NotoSans.ttf")
        target_link_libraries(draw_page_tests PRIVATE prax::font prax::path prax::gpu)
    endif()
    if (PR_DRAW_PDF)
        target_sources(draw_tests PRIVATE
            backends/pdf/tests/test_draw_pdf.cpp
            backends/pdf/tests/test_draw_pdf_c.c
        )
    endif()
    if (PR_DRAW_SVG)
        target_sources(draw_tests PRIVATE
            backends/svg/tests/test_draw_svg.cpp
            backends/svg/tests/test_draw_svg_c.c
        )
    endif()
    if (PR_DRAW_TRIANGLES OR PR_DRAW_FRAGMENT)
        target_sources(draw_tests PRIVATE
            backends/shared/tests/test_draw_shared_atlas.cpp
            backends/shared/tests/test_draw_shared_c.c
        )
    endif()
    if (PR_DRAW_TRIANGLES)
        target_sources(draw_tests PRIVATE
            backends/triangles/tests/test_draw_triangles_c.c
            backends/triangles/tests/test_draw_triangles_device.cpp
        )
    endif()
    if (PR_DRAW_FRAGMENT)
        target_sources(draw_tests PRIVATE
            backends/fragment/tests/test_draw_fragment_c.c
            backends/fragment/tests/test_draw_fragment_device.cpp
        )
    endif()
endif()
