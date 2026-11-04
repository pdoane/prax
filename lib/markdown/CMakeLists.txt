# Markdown: CommonMark and its extensions, parsed into a document that a client walks.
pr_library(markdown INCLUDE include
    src/markdown.cpp
    src/markdown_block.cpp
    src/markdown_html.cpp
    src/markdown_inline.cpp
    src/markdown_scan.cpp
    src/markdown_tree.cpp
)
target_link_libraries(pr_markdown PUBLIC prax::core PRIVATE prax::core_cpp prax::unicode prax::html_entity prax::tex)

if (PR_DEVELOP)
    pr_module_tests(markdown
        tests/test_markdown.cpp
        tests/test_markdown_c.c
    )
    # The CommonMark and GFM harness, which tools/suite/markdown/markdown_spec.py drives.
    pr_harness(markdown_spec prax::markdown tests/markdown_spec.cpp)
endif()
