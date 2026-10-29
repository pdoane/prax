# The shell, and what hosts pages for it and for the viewer: the JS bindings' programs.
if (PR_JS)
    # Local HTML pages and WPT's, fetched as a browser fetches them, run and rendered: what pr_shell and the viewer
    # host pages with.
    add_library(page_host STATIC page_host.cpp)
    target_include_directories(page_host PUBLIC ${PROJECT_SOURCE_DIR})
    target_compile_definitions(page_host PRIVATE PR_FONT_BUNDLE_DIR="${PR_FONT_BUNDLE_DIR}")
    target_link_libraries(page_host PUBLIC prax::app prax::draw
                                    PRIVATE prax::core_quickjs_cpp prax::html_quickjs
                                            prax::svg_quickjs prax::mathml_quickjs prax::url_quickjs
                                            prax::xhr_quickjs
                                            prax::charset_quickjs harness prax::font_bundle prax::image
                                            prax::compress prax::css prax::fetch prax::font
                                            prax::html prax::layout prax::tls prax::url prax::json
                                            pr_warnings)
    target_link_libraries(page_host PRIVATE prax::http prax::storage prax::net prax::task)

    add_executable(pr_shell shell.cpp)
    target_link_libraries(pr_shell PRIVATE harness_png harness_streams page_host prax::draw prax::text prax::font
                                           draw_compare harness pr_warnings)

    # The fragment renderer on the platform's device, which pr_shell renders with when asked.
    add_library(device_render STATIC device_render.cpp)
    target_include_directories(device_render PUBLIC ${PROJECT_SOURCE_DIR})
    target_link_libraries(device_render PUBLIC prax::draw PRIVATE prax::gpu pr_warnings)
    target_link_libraries(pr_shell PRIVATE device_render)
endif()
