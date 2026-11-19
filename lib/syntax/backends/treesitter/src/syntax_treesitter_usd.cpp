// The tree-sitter language of USD's text layers: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_usd.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_usd.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterUsd(void) {
    static const PrStringView kMediaTypes[] = {"model/vnd.usda"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "usd",
            .namePattern = "^(usd|usda)$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_usd(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
