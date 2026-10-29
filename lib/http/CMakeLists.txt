# HTTP: the interface, and the backend that the build turns on, HTTP/1.1 over lib/net and lib/tls.
pr_library(http INCLUDE include
    src/http.cpp
)
target_link_libraries(pr_http PUBLIC prax::net prax::tls PRIVATE prax::core_cpp)

if (PR_HTTP_BUILTIN)
    target_sources(pr_http PRIVATE backends/builtin/src/http_builtin.cpp)
    pr_headers(pr_http PUBLIC backends/builtin/include)
    target_compile_definitions(pr_http PRIVATE PR_HTTP_HAS_BUILTIN)
    target_link_libraries(pr_http PRIVATE prax::url)
endif()

if (PR_DEVELOP)
    pr_net_tests(http_tests tests/test_http.cpp tests/test_http_c.c)
    target_link_libraries(http_tests PRIVATE prax::http prax::tls)
endif()
