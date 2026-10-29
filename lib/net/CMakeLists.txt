# Networks: the interface, and the backend that the build turns on.
pr_library(net INCLUDE include
    src/net.cpp
)
target_link_libraries(pr_net PUBLIC prax::core PRIVATE prax::core_cpp)

# The network over the system's sockets: its loop is shared, and each platform's sockets are a source of their own,
# Winsock's on Windows and BSD sockets' elsewhere.
if (PR_NET_SOCKETS)
    target_sources(pr_net PRIVATE backends/sockets/src/net_sockets.cpp)
    pr_headers(pr_net PUBLIC backends/sockets/include)
    target_compile_definitions(pr_net PRIVATE PR_NET_HAS_SOCKETS)
    if (WIN32)
        target_sources(pr_net PRIVATE backends/sockets/src/net_sockets_platform_windows.cpp)
        target_link_libraries(pr_net PUBLIC ws2_32)
    else()
        target_sources(pr_net PRIVATE backends/sockets/src/net_sockets_platform_posix.cpp)
    endif()
endif()

if (PR_NET_NULL)
    target_sources(pr_net PRIVATE backends/null/src/net_null.cpp)
    pr_headers(pr_net PUBLIC backends/null/include)
    target_compile_definitions(pr_net PRIVATE PR_NET_HAS_NULL)
endif()

if (PR_DEVELOP)
    # The tests of lib/net, lib/tls, lib/http and lib/fetch run against the servers of tools/harness/net_peer.py, which runs
    # each binary whole, through its peer (tools/harness/net_peer.h). They run over the platform's sockets.

    # Adds the test binary `name` that tools/harness/net_peer.py runs.
    function(pr_net_tests name)
        add_executable(${name} ${ARGN})
        target_link_libraries(${name} PRIVATE net_peer prax::net prax::core_cpp test_main pr_warnings)
        add_test(NAME ${name} COMMAND ${Python3_EXECUTABLE} -m tools.harness.net_peer $<TARGET_FILE:${name}>
                 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(${name} PROPERTIES TIMEOUT 60)
    endfunction()

    pr_net_tests(net_tests tests/test_net.cpp tests/test_net_c.c)
    target_link_libraries(net_tests PRIVATE prax::net)
    if (PR_NET_NULL)
        target_sources(net_tests PRIVATE backends/null/tests/test_net_null.cpp)
    endif()
endif()
