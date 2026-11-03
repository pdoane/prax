# Debuggers: a client of the Debug Adapter Protocol, over a transport that its host provides.
pr_library(dap INCLUDE include
    src/dap.cpp
)
target_link_libraries(pr_dap PUBLIC prax::core prax::json PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(dap
        tests/test_dap.cpp
        tests/test_dap_c.c
    )
endif()
