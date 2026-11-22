# Basis Universal. The library decodes the ETC1S and UASTC formats to pixels and transcodes them to the blocks that a
# GPU samples. It also decodes those blocks to pixels.
pr_library(basis INCLUDE include
    src/basis.cpp
    src/basis_etc1s.cpp
    src/basis_uastc.cpp
)
target_link_libraries(pr_basis PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(basis
        tests/test_basis.cpp
        tests/test_basis_c.c
    )
endif()
