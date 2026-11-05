// The tree-sitter language of Markdown: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_markdown.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_markdown.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterMarkdown(void) {
    static const PrStringView kExtensions[] = {"md", "markdown"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "markdown",
            .namePattern = "^(markdown|md)$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_markdown(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
