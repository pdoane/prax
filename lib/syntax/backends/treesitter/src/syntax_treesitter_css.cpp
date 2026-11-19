// The tree-sitter language of CSS: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_css.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_css.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterCss(void) {
    static const PrStringView kMediaTypes[] = {"text/css"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "css",
            .namePattern = "^css$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_css(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
