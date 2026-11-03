// The tree-sitter language of C: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_c.h"

#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_c.h"

#include "tree_sitter/tree-sitter-c.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterC(void) {
    static const PrStringView kExtensions[] = {"c", "h"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "c",
            .namePattern = "^(c|h)$",
            .extensions = kExtensions,
            .extensionCount = 2,
            .grammar = tree_sitter_c(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
