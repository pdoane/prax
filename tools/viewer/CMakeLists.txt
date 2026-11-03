if ((APPLE OR WIN32) AND PR_JS)
    # The documents that the viewer shows and draw_bench times: the drawings made in code, and the corpora's documents.
    add_library(viewer_documents STATIC
        renderers.cpp
        document_tree.cpp
        documents.cpp
        source_image.cpp
        source_lottie.cpp
        source_svg.cpp
        suite_lottie.cpp
        suite_markdown.cpp
        suite_prax.cpp
        suite_svg.cpp
        suite_wpt.cpp
        suites.cpp
        drawing.cpp
        features.cpp
        features_boxes.cpp
        features_compose.cpp
        features_filters.cpp
        features_fonts.cpp
        features_paint.cpp
        features_shapes.cpp
        features_text.cpp
        panes.cpp
        workloads.cpp
        source_html.cpp
        source_rlottie.cpp
        # The renderers that only one platform has: Core Graphics on macOS.
        renderers_platform.cpp
    )
    target_include_directories(viewer_documents PUBLIC ${PROJECT_SOURCE_DIR})
    target_compile_definitions(viewer_documents PRIVATE
        PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts"
        PR_SUITES_DIR="${PROJECT_SOURCE_DIR}/suites"
        PR_THIRD_PARTY_DIR="${PROJECT_SOURCE_DIR}/third_party"
        PR_VIEWER_CACHE_DIR="${PROJECT_BINARY_DIR}/viewer-cache")
    target_link_libraries(viewer_documents PUBLIC prax::draw draw_compare suite_render prax::text prax::svg
                                                  draw_blend2d prax::image prax::app prax::compress prax::dom
                                                  prax::lottie prax::font
                                           PRIVATE prax::core_cpp prax::gpu prax::path prax::color prax::json page_host
                                                   draw_imgui rlottie::rlottie pr_warnings)
    if (APPLE)
        target_link_libraries(viewer_documents PRIVATE draw_coregraphics)
    endif()

    # The viewer: the documents, rendered by each renderer, beside its controls in Studio Kit.
    add_executable(viewer viewer.cpp ui.cpp)
    target_compile_definitions(viewer PRIVATE PR_FONT_BUNDLE_DIR="${PR_FONT_BUNDLE_DIR}")
    target_link_libraries(viewer PRIVATE viewer_documents prax::app prax::draw prax::font prax::font_bundle prax::geom
                                         prax::gpu prax::compress prax::image prax::layout prax::studio_kit prax::text
                                         prax::ui prax::core_cpp pr_warnings)
endif()
