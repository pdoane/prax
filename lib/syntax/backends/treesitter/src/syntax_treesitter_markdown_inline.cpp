// The tree-sitter language of Markdown's inline text: its grammar, and the queries that the build reads from its
// package.

#include "pr/syntax_treesitter_markdown_inline.h"

#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_markdown_inline.h"

#include "tree-sitter/tree-sitter-markdown-inline.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterMarkdownInline(void) {
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "markdown_inline",
            .grammar = tree_sitter_markdown_inline(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
