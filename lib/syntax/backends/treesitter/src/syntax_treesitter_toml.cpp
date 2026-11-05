// The tree-sitter language of TOML: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_toml.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_toml.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterToml(void) {
    static const PrStringView kExtensions[] = {"toml"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "toml",
            .namePattern = "^toml$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_toml(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
