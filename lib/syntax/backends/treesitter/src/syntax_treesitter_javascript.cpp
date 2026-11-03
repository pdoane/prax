// The tree-sitter language of JavaScript: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_javascript.h"

#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_javascript.h"

#include "tree_sitter/tree-sitter-javascript.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterJavaScript(void) {
    static const PrStringView kExtensions[] = {"js", "mjs", "cjs", "jsx"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "javascript",
            .namePattern = "^(js|javascript)$",
            .extensions = kExtensions,
            .extensionCount = 4,
            .grammar = tree_sitter_javascript(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
