# Drawing: draw lists, and the backends that render them or write them as documents. Each backend is built on the
# public API of draw and path. The selector (select/) creates a renderer or a writer of a given backend.
pr_library(draw INCLUDE include
    src/draw.cpp
    src/draw_commands.cpp
    src/draw_display.cpp
    src/draw_filter.cpp
    src/draw_glyph.cpp
    src/identity.cpp
    src/draw_list.cpp
    src/draw_lower.cpp
    src/draw_projection.cpp
    src/draw_renderer.cpp
    src/draw_structure.cpp
    src/draw_transform.cpp
    src/draw_writer.cpp
    backends/shared/src/draw_shared_memory.cpp
)
target_link_libraries(pr_draw PUBLIC prax::path prax::font prax::image prax::texture prax::geom prax::gpu prax::color
                                     prax::compress prax::task
                              PRIVATE prax::core_cpp)
# The display shader, which maps a frame of lib/draw's colors to a display (pr/draw_display.h).
pr_slang(pr_draw src/draw_display.slang VARIABLE kDrawDisplayShaders)

# Code that backends share. The GPU renderers keep glyphs and paths in an atlas. The writers share a walk of the draw
# list that writes what the format supports, lowers some commands, and rasterizes the rest with the CPU renderer. The
# memory frames that CPU renderers draw into are part of the module's library, because the comparisons use them too.
pr_backend_shared(draw atlas SOURCES backends/shared/src/draw_shared_atlas.cpp INCLUDE backends/shared/include)
pr_backend_shared(draw writer SOURCES backends/shared/src/draw_shared_writer.cpp)

# Renderers.
pr_backend(draw cpu INCLUDE backends/cpu/include SOURCES
    backends/cpu/src/draw_cpu.cpp
    backends/cpu/src/cpu_filter.cpp
    backends/cpu/src/cpu_pixels.cpp
)
# The Canvas 2D renderer draws through a web page's canvases, so it builds only for the web.
pr_backend(draw canvas WHEN PR_GPU_CANVAS2D INCLUDE backends/canvas/include SOURCES backends/canvas/src/draw_canvas.cpp)
pr_backend(draw triangles SHARED atlas INCLUDE backends/triangles/include SOURCES
    backends/triangles/src/draw_triangles.cpp
    backends/triangles/src/draw_triangles_device.cpp
)
pr_backend(draw fragment SHARED atlas INCLUDE backends/fragment/include SOURCES
    backends/fragment/src/draw_fragment.cpp
    backends/fragment/src/draw_fragment_device.cpp
)

# Writers. A writer rasterizes what its format cannot express with the CPU renderer.
pr_backend(draw pdf SHARED writer REQUIRES cpu INCLUDE backends/pdf/include SOURCES backends/pdf/src/draw_pdf.cpp)
pr_backend(draw svg SHARED writer REQUIRES cpu INCLUDE backends/svg/include SOURCES backends/svg/src/draw_svg.cpp)

# Build steps of backends other than compiling their sources.
pr_backend_built(draw canvas PR_DRAW_BUILT_CANVAS)
if (PR_DRAW_BUILT_CANVAS)
    pr_js_library(pr_draw backends/canvas/src/draw_canvas.js)
endif()
if (PR_DRAW_FRAGMENT)
    # The fragment renderer's shaders share the frame's constants and records with its C++ (draw_fragment_constants.h
    # and draw_fragment_records.h). They compile once for devices with bindless, and once for the slots alone.
    pr_slang(pr_draw backends/fragment/src/draw_fragment.slang VARIABLE kDrawFragmentShaders
        INCLUDE ${PROJECT_SOURCE_DIR}/lib)
    pr_slang(pr_draw backends/fragment/src/draw_fragment.slang VARIABLE kDrawFragmentSlotShaders
        INCLUDE ${PROJECT_SOURCE_DIR}/lib DEFINES PR_DRAW_FRAGMENT_SLOTS)
endif()
if (PR_DRAW_TRIANGLES)
    pr_slang(pr_draw backends/triangles/src/draw_triangles.slang VARIABLE kDrawTrianglesShaders)
endif()

# The selector for renderer and writer backends. Programs and composition libraries link it.
pr_select(draw INCLUDE select/include SOURCES select/src/draw_select.cpp LINK prax::gpu)

if (PR_DEVELOP)
    # The tools compare lib/draw with the comparisons, so a build without the tools has no comparisons.
    if (PR_TOOLS)
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
    target_compile_definitions(draw_tests PRIVATE PR_FONTS_DIR="${PR_TEST_FONTS_DIR}")
    target_link_libraries(draw_tests PRIVATE prax::compress_select prax::image_select prax::gpu_select prax::texture
                                             prax::draw_select draw_compare prax::path
                                             prax::color
                                             prax::image prax::compress
                                             prax::font prax::gpu prax::task prax::svg)
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
            PRELOAD "${PR_TEST_FONTS_DIR}/NotoSans[wdth,wght].ttf@/fonts/NotoSans[wdth,wght].ttf")
        target_link_libraries(draw_page_tests PRIVATE prax::font prax::path prax::gpu prax::gpu_select)
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
            tests/test_draw_display.cpp
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
    # On the web, the GPU renderers also run their tests in a page, on the page's WebGPU.
    if (TARGET draw_page_tests)
        if (PR_DRAW_TRIANGLES)
            target_sources(draw_page_tests PRIVATE backends/triangles/tests/test_draw_triangles_c.c
                                                  backends/triangles/tests/test_draw_triangles_device.cpp)
        endif()
        if (PR_DRAW_FRAGMENT)
            target_sources(draw_page_tests PRIVATE backends/fragment/tests/test_draw_fragment_c.c
                                                  backends/fragment/tests/test_draw_fragment_device.cpp)
        endif()
        target_compile_definitions(draw_page_tests PRIVATE PR_FONTS_DIR="/fonts")
        target_link_libraries(draw_page_tests PRIVATE draw_compare prax::color prax::image prax::compress)
    endif()
endif()
