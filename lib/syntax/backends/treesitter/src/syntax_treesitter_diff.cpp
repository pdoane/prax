// The tree-sitter language of a diff: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_diff.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_diff.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterDiff(void) {
    static const PrStringView kExtensions[] = {"diff", "patch"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "diff",
            .namePattern = "^(diff|patch)$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_diff(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
