// The tree-sitter language of HTML: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_html.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_html.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterHtml(void) {
    static const PrStringView kMediaTypes[] = {"text/html", "application/xhtml+xml"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "html",
            .namePattern = "^html$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_html(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
