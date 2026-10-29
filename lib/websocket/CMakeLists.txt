# WebSockets: the interface, and the backend that the build turns on, the WebSocket protocol over lib/net and lib/tls.
pr_library(websocket INCLUDE include
    src/websocket.cpp
)
target_link_libraries(pr_websocket PUBLIC prax::net prax::tls PRIVATE prax::core_cpp)

if (PR_WEBSOCKET_BUILTIN)
    target_sources(pr_websocket PRIVATE backends/builtin/src/websocket_builtin.cpp)
    pr_headers(pr_websocket PUBLIC backends/builtin/include)
    target_compile_definitions(pr_websocket PRIVATE PR_WEBSOCKET_HAS_BUILTIN)
    target_link_libraries(pr_websocket PRIVATE prax::url)
endif()

if (PR_DEVELOP)
    pr_net_tests(websocket_tests tests/test_websocket.cpp tests/test_websocket_c.c)
    target_link_libraries(websocket_tests PRIVATE prax::websocket prax::tls)
endif()
