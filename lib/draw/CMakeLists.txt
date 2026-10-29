# Drawing: draw lists, and the backends that the build turns on, which render them and decode glyphs. A backend is built
# on the public API of draw and path.
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
    src/draw_transform.cpp
)
target_link_libraries(pr_draw PUBLIC prax::path prax::font prax::image prax::geom prax::gpu prax::color
                              PRIVATE prax::core_cpp)
# The display shader, which maps a frame of lib/draw's colors to a display (pr/draw_display.h).
pr_slang(pr_draw src/draw_display.slang VARIABLE kDrawDisplayShaders)

if (PR_DRAW_GLYPH_BITMAP)
    target_sources(pr_draw PRIVATE backends/glyph_bitmap/src/draw_glyph_bitmap.cpp)
    pr_headers(pr_draw PUBLIC backends/glyph_bitmap/include)
endif()

if (PR_DRAW_CPU)
    target_sources(pr_draw PRIVATE
        backends/cpu/src/draw_cpu.cpp
        backends/cpu/src/cpu_filter.cpp
        backends/cpu/src/cpu_pixels.cpp
    )
    pr_headers(pr_draw PUBLIC backends/cpu/include)
    target_compile_definitions(pr_draw PRIVATE PR_DRAW_HAS_CPU)
endif()

# What the backends share: the atlas that the GPU renderers keep glyphs and paths in.
if (PR_DRAW_TRIANGLES OR PR_DRAW_FRAGMENT)
    target_sources(pr_draw PRIVATE backends/shared/src/draw_shared_atlas.cpp)
    pr_headers(pr_draw PUBLIC backends/shared/include)
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
    add_subdirectory(comparisons)

    pr_module_tests(draw
        tests/test_compare.cpp
        tests/test_draw_c.c
        tests/test_draw_list.cpp
        tests/test_draw_lower.cpp
        tests/test_draw_projection.cpp
        tests/test_draw_text.cpp
    )
    target_compile_definitions(draw_tests PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")
    target_link_libraries(draw_tests PRIVATE draw_compare prax::path prax::color prax::image prax::compress
                                             prax::font prax::gpu)
    if (PR_DRAW_GLYPH_BITMAP)
        target_sources(draw_tests PRIVATE
            backends/glyph_bitmap/tests/test_draw_glyph_bitmap.cpp
            backends/glyph_bitmap/tests/test_draw_glyph_bitmap_c.c
        )
    endif()
    if (PR_DRAW_CPU)
        target_sources(draw_tests PRIVATE
            backends/cpu/tests/test_draw_cpu.cpp
            backends/cpu/tests/test_draw_cpu_c.c
            backends/cpu/tests/test_draw_cpu_text.cpp
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
