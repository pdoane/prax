// The tree-sitter language of WGSL: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_wgsl.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_wgsl.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterWgsl(void) {
    static const PrStringView kExtensions[] = {"wgsl"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "wgsl",
            .namePattern = "^wgsl$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_wgsl(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
