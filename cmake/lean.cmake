# A lean build (PR_LEAN): a development build of the tests and what they drive, which fetches only what they read, as
# ./dev.sh ci builds (tools/build/build.py). It has no tools or comparisons and no examples, which have no tests. Of the
# font bundles, it fetches Standard14, which pr_shell's pages set text in; the tests read the bundles' files that they
# need from the test data (cmake/test_data.cmake). Of the tree-sitter languages, it builds in those that lib/syntax's
# tests name and those that they inject. The tests of each other language's grammar run in a build of every language.
#
# What a lean build fetches is decided here, beside the pins of what it fetches, and these values replace the options
# that they name.
if (PR_LEAN)
    set(PR_TOOLS OFF)
    set(PR_EXAMPLES OFF)
    set(PR_FONT_BUNDLES Standard14)
    set(PR_SYNTAX_LANGUAGES c cpp css html javascript json markdown markdown_inline python xml)
endif()
