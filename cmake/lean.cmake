# A lean build (PR_LEAN): a development build of the tests and what they drive, which fetches only what they read, as
# ./dev.sh ci builds (tools/build/build.py). It has no tools or comparisons and no examples, which have no tests. Of the
# font bundles, it fetches Standard14, which pr_shell's pages set text in; the tests read the bundles' files that they
# need from the test data (cmake/test_data.cmake). It leaves out lib/syntax's tree-sitter languages, whose grammars are
# much of a build's compile and are tested by tree-sitter's own projects; their tests run in a full development build.
#
# What a lean build fetches is decided here, beside the pins of what it fetches, and these values replace the options
# that they name.
if (PR_LEAN)
    set(PR_TOOLS OFF)
    set(PR_EXAMPLES OFF)
    set(PR_FONT_BUNDLES Standard14)
    set(PR_SYNTAX_TREESITTER OFF)
endif()
