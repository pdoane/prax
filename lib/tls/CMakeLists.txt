# TLS: the interface, and the backends that the build turns on.
pr_library(tls INCLUDE include
    src/tls.cpp
)
target_link_libraries(pr_tls PUBLIC prax::net PRIVATE prax::core_cpp)

# What the backends that run an engine over a transport share: OpenSSL's and SChannel's.
if (PR_TLS_SCHANNEL OR PR_TLS_OPENSSL)
    target_sources(pr_tls PRIVATE backends/shared/src/tls_shared.cpp)
endif()

if (PR_TLS_APPLE)
    target_sources(pr_tls PRIVATE backends/apple/src/tls_apple.cpp)
    pr_headers(pr_tls PUBLIC backends/apple/include)
    target_compile_definitions(pr_tls PRIVATE PR_TLS_HAS_APPLE)
    target_link_libraries(pr_tls PUBLIC "-framework Network" "-framework Security" "-framework CoreFoundation")
endif()

if (PR_TLS_SCHANNEL)
    target_sources(pr_tls PRIVATE backends/schannel/src/tls_schannel.cpp)
    pr_headers(pr_tls PUBLIC backends/schannel/include)
    target_compile_definitions(pr_tls PRIVATE PR_TLS_HAS_SCHANNEL)
    target_link_libraries(pr_tls PUBLIC secur32 crypt32 PRIVATE prax::core_windows)
endif()

# The OpenSSL backend is written against OpenSSL 3.
if (PR_TLS_OPENSSL)
    find_package(OpenSSL 3...<4 REQUIRED)
    target_sources(pr_tls PRIVATE backends/openssl/src/tls_openssl.cpp)
    pr_headers(pr_tls PUBLIC backends/openssl/include)
    target_compile_definitions(pr_tls PRIVATE PR_TLS_HAS_OPENSSL)
    target_link_libraries(pr_tls PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

# Its tests run over the platform's sockets (lib/net).
if (PR_DEVELOP AND PR_NET_SOCKETS)
    pr_net_tests(tls_tests tests/test_tls.cpp tests/test_tls_c.c)
    target_link_libraries(tls_tests PRIVATE prax::tls)
endif()
