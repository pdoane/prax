# Basis Universal. The library decodes the ETC1S and UASTC formats to pixels and transcodes them to the blocks that a
# GPU samples. lib/bc and lib/etc2 decode those blocks.
pr_library(basis INCLUDE include
    src/basis_etc1s.cpp
    src/basis_uastc.cpp
)
target_link_libraries(pr_basis PUBLIC prax::core PRIVATE prax::bc prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(basis
        tests/test_basis.cpp
        tests/test_basis_c.c
    )
endif()
