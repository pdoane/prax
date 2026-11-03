# XHR: XMLHttpRequest, a script's requests through its client, and its QuickJS layer, whose client is a window.
pr_library(xhr INCLUDE include
    src/xhr.cpp
    src/xhr_file_reader.cpp
)
target_link_libraries(pr_xhr PUBLIC prax::dom prax::fetch PRIVATE prax::core_cpp prax::charset prax::mime prax::url)

if (PR_JS)
    pr_library(xhr_quickjs INCLUDE quickjs/include
        quickjs/src/xhr_quickjs.cpp
        quickjs/src/xhr_file_reader_quickjs.cpp
    )
    target_link_libraries(pr_xhr_quickjs PUBLIC prax::xhr prax::quickjs PRIVATE prax::dom prax::dom_quickjs
                                         prax::fetch_quickjs prax::html
                                         prax::url_quickjs prax::webidl_quickjs_cpp prax::core_quickjs_cpp)
endif()

if (PR_DEVELOP)
    pr_module_tests(xhr tests/test_xhr.cpp tests/test_xhr_c.c)
endif()
