// The tree-sitter language of Kotlin: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_kotlin.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_kotlin.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterKotlin(void) {
    static const PrStringView kMediaTypes[] = {"text/x-kotlin"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "kotlin",
            .namePattern = "^(kotlin|kt)$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_kotlin(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
