// The tree-sitter language of C#: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_c_sharp.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_c_sharp.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterCSharp(void) {
    static const PrStringView kExtensions[] = {"cs", "csx"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "csharp",
            .namePattern = "^(cs|csharp|c#)$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_c_sharp(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
