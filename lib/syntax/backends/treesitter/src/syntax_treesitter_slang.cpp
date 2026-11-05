// The tree-sitter language of Slang: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_slang.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_slang.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterSlang(void) {
    static const PrStringView kExtensions[] = {"slang"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "slang",
            .namePattern = "^slang$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_slang(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
