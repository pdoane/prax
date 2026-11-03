# HTML: its elements, parsing, and the Window, which runs documents' scripts in the QuickJS layer.
pr_library(html INCLUDE include
    src/html_page.cpp
    src/html.cpp
    src/html_boxes.cpp
    src/html_canvas.cpp
    src/html_adjust.cpp
    src/html_charset.cpp
    src/html_document.cpp
    src/html_error.cpp
    src/html_focus.cpp
    src/html_forms.cpp
    src/html_message.cpp
    src/html_performance.cpp
    src/html_history.cpp
    src/html_hints.cpp
    src/html_image.cpp
    src/html_image_source.cpp
    src/html_link.cpp
    src/html_markdown.cpp
    src/html_parser.cpp
    src/html_serialize.cpp
    src/html_storage.cpp
    src/html_table.cpp
    src/html_tokenizer.cpp
    src/html_media.cpp
    src/html_media_source.cpp
    src/html_view.cpp
    src/html_window.cpp
    src/html_xml.cpp
)
target_link_libraries(pr_html PUBLIC prax::app prax::audio prax::color prax::canvas prax::gpu prax::charset prax::css prax::dom
                                      prax::fetch prax::svg prax::url prax::image prax::media prax::draw prax::layout
                                     prax::markdown prax::storage prax::task
                              PRIVATE prax::core_cpp prax::mathml prax::net prax::html_entity prax::text prax::locale prax::mime)

if (PR_JS)
    pr_library(html_quickjs INCLUDE quickjs/include
        quickjs/src/custom_elements_quickjs.cpp
        quickjs/src/html_bindings_quickjs.cpp
        quickjs/src/html_clone_quickjs.cpp
        quickjs/src/html_elements_quickjs.cpp
        quickjs/src/html_forms_quickjs.cpp
        quickjs/src/html_media_source_quickjs.cpp
        quickjs/src/html_message_quickjs.cpp
        quickjs/src/html_performance_quickjs.cpp
        quickjs/src/html_console_quickjs.cpp
        quickjs/src/html_quickjs.cpp
    )
    target_link_libraries(pr_html_quickjs PUBLIC prax::html prax::quickjs prax::css_quickjs
                                                 prax::dom_quickjs PRIVATE
                                          prax::canvas_quickjs prax::core_quickjs_cpp prax::dom prax::fetch_quickjs
                                          prax::webidl_quickjs_cpp prax::url)
endif()

if (PR_DEVELOP)
    # What the tests of lib/html host without a script engine: a fetcher that they answer, and modules that they parse.
    add_library(html_test_fetch STATIC tests/support/html_test_fetch.cpp)
    target_link_libraries(html_test_fetch PUBLIC prax::dom prax::html prax::fetch prax::net
                                          PRIVATE prax::core_cpp pr_warnings prax::url)
    pr_module_tests(html
        tests/test_html_page.cpp
        tests/test_html_c.c
        tests/test_html_charset.cpp
        tests/test_html_forms.cpp
        tests/test_html_markdown.cpp
        tests/test_fonts.cpp
        tests/test_parser.cpp
        tests/test_html_render.cpp
        tests/test_html_storage.cpp
        tests/test_html_style.cpp
        tests/test_html_view.cpp
        tests/test_timer.cpp
        tests/test_tokenizer.cpp
        tests/test_html_fetch_thread.cpp
    )
    # Rendering sets text with HarfBuzz in the fonts the checkout holds. The fetches of a browsing context run on a
    # network whose loop is a thread of its own.
    target_link_libraries(html_tests PRIVATE prax::css  prax::text prax::image prax::compress
                                             prax::draw prax::dom prax::font prax::layout
                                             prax::markdown html_test_fetch prax::task prax::locale
                                             prax::net prax::fetch)
    target_compile_definitions(html_tests PRIVATE
        PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts"
        PR_HTML5LIB_DIR="${PROJECT_SOURCE_DIR}/third_party/html5lib"
        PR_HTML_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}/html_tests_files")

    # The Markdown DOM harness, which tools/suite/markdown/markdown_dom.py drives.
    pr_harness(markdown_dom prax::html tests/markdown_dom.cpp)
    target_link_libraries(markdown_dom PRIVATE prax::markdown prax::dom)

    # The html5lib tokenizer harness, which tools/suite/html_tokens.py drives.
    pr_harness(html_tokens prax::html tests/html_tokens.cpp)

    # The tree construction harness, which tools/suite/html_tree.py drives.
    pr_harness(html_tree prax::html tests/html_tree.cpp)

    # The engine's own HTML benchmark, which ./dev.sh bench html-parse runs beside a browser's.
    pr_harness(html_bench prax::html tests/html_bench.cpp)

    # The mutation benchmark's cases through the C API, which ./dev.sh bench mutation runs beside the shell's.
    pr_harness(mutation_bench prax::html tests/mutation_bench.cpp)
    target_link_libraries(mutation_bench PRIVATE prax::css)

    # The style benchmark's cases through the C API, which ./dev.sh bench restyle runs beside the shell's.
    pr_harness(style_bench prax::html tests/style_bench.cpp)
    target_link_libraries(style_bench PRIVATE prax::text prax::css prax::layout)
    target_compile_definitions(style_bench PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")

    # What a frame costs after each kind of change, and loading a page in pieces, which ./dev.sh bench frame runs.
    pr_harness(frame_bench prax::html tests/frame_bench.cpp)
    target_link_libraries(frame_bench PRIVATE prax::text prax::css  prax::layout)
    target_compile_definitions(frame_bench PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")
    pr_harness(list_bench prax::html tests/list_bench.cpp)
    target_link_libraries(list_bench PRIVATE prax::text prax::css prax::layout)
    target_compile_definitions(list_bench PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")

    # The selector benchmark's cases through the C API, which ./dev.sh bench selectors runs beside the shell's.
    pr_harness(selector_bench prax::html tests/selector_bench.cpp)

    if (PR_JS)
        pr_module_tests(html_quickjs
            quickjs/tests/test_agents_quickjs.cpp
            quickjs/tests/test_collection_quickjs.cpp
            quickjs/tests/test_html_quickjs_c.c
            quickjs/tests/test_lifetime_quickjs.cpp
            quickjs/tests/test_style_quickjs.cpp
        )
        # test_agents_quickjs runs agents on their own threads.
        find_package(Threads REQUIRED)
        target_link_libraries(html_quickjs_tests PRIVATE prax::canvas_quickjs prax::charset_quickjs prax::css_quickjs
                                                         prax::core_quickjs_cpp prax::fetch_quickjs prax::xhr_quickjs
                                                         prax::dom prax::dom_quickjs prax::fetch prax::html prax::net
                                                         prax::svg_quickjs prax::mathml_quickjs prax::url_quickjs
                                                         prax::webidl_quickjs_cpp Threads::Threads)
        target_compile_definitions(html_quickjs_tests PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")
        add_test(NAME html_host_scripts COMMAND ${Python3_EXECUTABLE} -m tools.shell.test_shell
                 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(html_host_scripts PROPERTIES ENVIRONMENT "PR_SHELL=$<TARGET_FILE:pr_shell>" TIMEOUT 60)
        add_test(NAME html_js_timers COMMAND pr_shell --html ${CMAKE_CURRENT_SOURCE_DIR}/tests/timers.html)
        set_tests_properties(html_js_timers PROPERTIES PASS_REGULAR_EXPRESSION "timer host tests passed" TIMEOUT 10)
    endif()
endif()
