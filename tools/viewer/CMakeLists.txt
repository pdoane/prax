if ((APPLE OR WIN32) AND PR_JS)
    # The documents that the viewer shows: the corpora's documents and their references.
    add_library(viewer_documents STATIC
        document_tree.cpp
        documents.cpp
        source_lottie.cpp
        suite_lottie.cpp
        suite_markdown.cpp
        suite_mathml.cpp
        suite_prax.cpp
        suite_prince.cpp
        suite_svg.cpp
        suite_wpt.cpp
        panes.cpp
        source_html.cpp
        source_pdf.cpp
        source_rlottie.cpp
    )
    target_include_directories(viewer_documents PUBLIC ${PROJECT_SOURCE_DIR})
    target_compile_definitions(viewer_documents PRIVATE
        PR_SUITES_DIR="${PROJECT_SOURCE_DIR}/suites"
        PR_TEX_TESTS_DIR="${PROJECT_SOURCE_DIR}/lib/tex/tests"
        PR_THIRD_PARTY_DIR="${PROJECT_SOURCE_DIR}/third_party"
        PR_VIEWER_CACHE_DIR="${PROJECT_BINARY_DIR}/viewer-cache")
    target_link_libraries(viewer_documents PUBLIC prax::draw draw_compare draw_renderers suite_render prax::text
                                                  prax::image prax::app prax::compress prax::lottie prax::font
                                                  page_host
                                           PRIVATE prax::core_cpp prax::gpu prax::json prax::html prax::css
                                                   prax::layout rlottie::rlottie pr_warnings)
    if (APPLE)
        target_link_libraries(viewer_documents PRIVATE "-framework CoreGraphics")
    endif()

    # The viewer: the documents, rendered by each renderer, beside its controls in Studio Kit.
    add_executable(viewer viewer.cpp ui.cpp)
    target_compile_definitions(viewer PRIVATE PR_FONT_BUNDLE_DIR="${PR_FONT_BUNDLE_DIR}")
    target_link_libraries(viewer PRIVATE viewer_documents prax::app prax::webview prax::draw prax::font
                                         prax::font_bundle prax::geom prax::gpu prax::compress prax::image prax::layout
                                         prax::studio_kit prax::text prax::ui prax::core_cpp pr_warnings)

    # The tests of the viewer's own logic.
    add_executable(viewer_tests tests/test_zoom.cpp)
    target_link_libraries(viewer_tests PRIVATE test_main prax::core prax::core_cpp pr_warnings)
    pr_add_tests(viewer_tests)
endif()
