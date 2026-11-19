// The tree-sitter language of PHP: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_php.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_php.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterPhp(void) {
    static const PrStringView kMediaTypes[] = {"text/x-php", "application/x-php"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "php",
            .namePattern = "^php$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_php(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
