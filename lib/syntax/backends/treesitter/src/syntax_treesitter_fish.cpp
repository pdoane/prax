// The tree-sitter language of fish, the shell: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_fish.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_fish.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterFish(void) {
    static const PrStringView kMediaTypes[] = {"text/x-fish"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "fish",
            .namePattern = "^fish$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_fish(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
