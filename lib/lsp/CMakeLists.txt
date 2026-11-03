# Language servers: a client of the Language Server Protocol, over a transport that its host provides.
pr_library(lsp INCLUDE include
    src/lsp.cpp
)
target_link_libraries(pr_lsp PUBLIC prax::core prax::json prax::text_edit PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(lsp
        tests/test_lsp.cpp
        tests/test_lsp_c.c
    )
endif()
