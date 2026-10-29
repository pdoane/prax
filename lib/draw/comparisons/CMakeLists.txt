# The comparisons: backends that wrap other implementations, which the viewer, the tests and the benchmarks compare
# lib/draw with. They are built for development, not shipped.

# Dear ImGui, with its draw data drawn on lib/gpu, and the draw backend that records into it. The viewer draws its
# controls with the same build of Dear ImGui.
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    imgui/config/imgui_config.cpp
    imgui/config/imgui_gpu.cpp
)
pr_third_party_sources(${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
                       ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp)
target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR})
# Each thread has its own current context. Every user of Dear ImGui is built to agree on this.
target_compile_definitions(imgui PUBLIC IMGUI_USER_CONFIG="draw/comparisons/imgui/config/imgui_config.h")
target_link_libraries(imgui PUBLIC prax::gpu PRIVATE prax::core_cpp pr_warnings)
pr_slang(imgui imgui/config/imgui_gpu.slang VARIABLE kImGuiGpuShaders)

add_library(draw_imgui STATIC
    imgui/src/draw_imgui.cpp
    imgui/src/draw_imgui_gpu.cpp
)
pr_headers(draw_imgui PUBLIC imgui/include)
target_link_libraries(draw_imgui PUBLIC prax::draw prax::gpu
                                 PRIVATE prax::core_cpp imgui prax::font freetype prax::path pr_warnings)

if (APPLE)
    add_library(draw_coregraphics STATIC
        coregraphics/src/draw_coregraphics.cpp
    )
    pr_headers(draw_coregraphics PUBLIC coregraphics/include)
    target_link_libraries(draw_coregraphics PUBLIC prax::draw "-framework CoreGraphics" "-framework CoreText"
                                            "-framework CoreFoundation" PRIVATE prax::core_cpp pr_warnings)
endif()

# Blend2D, without its JIT, and the backend built on it. A Debug build optimizes it anyway, as it does mimalloc,
# because the viewer renders through it. MSVC refuses to optimize a Debug build's runtime checks, so it stays
# unoptimized there.
set(BLEND2D_STATIC ON CACHE BOOL "" FORCE)
set(BLEND2D_NO_JIT ON CACHE BOOL "" FORCE)
set(BLEND2D_NO_INSTALL ON CACHE BOOL "" FORCE)
set(BLEND2D_TEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(blend2d)
pr_third_party_directory(${blend2d_SOURCE_DIR})
if (CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT MSVC)
    target_compile_options(blend2d PRIVATE -O2)
endif()

add_library(draw_blend2d STATIC
    blend2d/src/draw_blend2d.cpp
)
pr_headers(draw_blend2d PUBLIC blend2d/include)
target_link_libraries(draw_blend2d PUBLIC prax::draw PRIVATE prax::core_cpp blend2d::blend2d prax::path pr_warnings)

pr_module_tests(draw_imgui
    imgui/tests/test_draw_imgui.cpp
)
target_compile_definitions(draw_imgui_tests PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")
target_link_libraries(draw_imgui_tests PRIVATE prax::draw prax::gpu)

pr_module_tests(draw_blend2d
    blend2d/tests/test_draw_blend2d.cpp
)
target_link_libraries(draw_blend2d_tests PRIVATE prax::draw)

if (APPLE)
    pr_module_tests(draw_coregraphics
        coregraphics/tests/test_draw_coregraphics.cpp
    )
    target_link_libraries(draw_coregraphics_tests PRIVATE prax::draw prax::text)
    target_compile_definitions(draw_coregraphics_tests PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")
endif()
