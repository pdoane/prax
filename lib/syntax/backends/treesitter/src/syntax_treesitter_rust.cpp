// The tree-sitter language of Rust: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_rust.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_rust.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterRust(void) {
    static const PrStringView kExtensions[] = {"rs"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "rust",
            .namePattern = "^(rs|rust)$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_rust(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
