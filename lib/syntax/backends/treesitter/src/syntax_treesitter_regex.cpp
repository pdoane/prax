// The tree-sitter language of regular expressions: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_regex.h"

#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_regex.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterRegex(void) {
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "regex",
            .namePattern = "^(regex|regexp)$",
            .grammar = tree_sitter_regex(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
