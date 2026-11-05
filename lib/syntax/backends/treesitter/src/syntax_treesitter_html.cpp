// The tree-sitter language of HTML: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_html.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_html.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterHtml(void) {
    static const PrStringView kExtensions[] = {"html", "htm", "xhtml"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "html",
            .namePattern = "^html$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_html(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
