# Fetch: the interface, the backends that the build turns on, and its QuickJS layer.
pr_library(fetch INCLUDE include
    src/fetch.cpp
    src/fetch_blob.cpp
    src/fetch_load.cpp
)
target_link_libraries(pr_fetch PUBLIC prax::net prax::http prax::compress prax::storage PRIVATE prax::core_cpp prax::mime)

if (PR_FETCH_BUILTIN)
    target_sources(pr_fetch PRIVATE
        backends/builtin/src/fetch_builtin.cpp
        backends/builtin/src/fetch_cache.cpp
    )
    pr_headers(pr_fetch PUBLIC backends/builtin/include)
    target_compile_definitions(pr_fetch PRIVATE PR_FETCH_HAS_BUILTIN)
    target_link_libraries(pr_fetch PRIVATE prax::url)
endif()

if (PR_FETCH_INTERCEPT)
    target_sources(pr_fetch PRIVATE backends/intercept/src/fetch_intercept.cpp)
    pr_headers(pr_fetch PUBLIC backends/intercept/include)
    target_link_libraries(pr_fetch PRIVATE prax::url)
endif()

if (PR_JS)
    pr_library(fetch_quickjs INCLUDE quickjs/include
        quickjs/src/fetch_blob_quickjs.cpp
        quickjs/src/fetch_quickjs.cpp
    )
    target_link_libraries(pr_fetch_quickjs PUBLIC prax::fetch prax::quickjs PRIVATE prax::charset prax::dom
                                           prax::dom_quickjs prax::url prax::url_quickjs prax::webidl_quickjs_cpp
                                           prax::core_quickjs_cpp)
endif()

# Its tests run over the platform's sockets (lib/net).
if (PR_DEVELOP AND PR_NET_SOCKETS)
    pr_net_tests(fetch_tests tests/test_fetch.cpp tests/test_fetch_c.c)
    target_link_libraries(fetch_tests PRIVATE prax::fetch prax::http prax::compress prax::storage
                                              prax::task)
    if (PR_FETCH_INTERCEPT)
        target_sources(fetch_tests PRIVATE backends/intercept/tests/test_fetch_intercept.cpp)
    endif()
    target_compile_definitions(fetch_tests PRIVATE PR_FETCH_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}/fetch_tests_files")
endif()
