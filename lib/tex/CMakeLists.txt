# TeX math: formulas in the TeX language of LaTeX, amsmath, MathJax, KaTeX and Temml, written as MathML Core.
pr_library(tex INCLUDE include
    src/tex.cpp
    src/tex_dictionary.cpp
    src/tex_parse.cpp
    src/tex_spacing.cpp
    src/tex_symbols.cpp
)
target_link_libraries(pr_tex PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(tex
        tests/test_tex.cpp
        tests/test_tex_c.c
    )
    target_compile_definitions(tex_tests PRIVATE TEX_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
endif()
