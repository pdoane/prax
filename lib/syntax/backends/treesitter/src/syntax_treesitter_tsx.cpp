// The tree-sitter language of TSX: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_tsx.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_tsx.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterTsx(void) {
    static const PrStringView kExtensions[] = {"tsx"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "tsx",
            .namePattern = "^tsx$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_tsx(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
