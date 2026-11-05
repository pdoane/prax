// The tree-sitter language of Go: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_go.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_go.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterGo(void) {
    static const PrStringView kExtensions[] = {"go"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "go",
            .namePattern = "^(go|golang)$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_go(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
