// The tree-sitter language of HLSL: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_hlsl.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_hlsl.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterHlsl(void) {
    static const PrStringView kExtensions[] = {"hlsl", "hlsli", "fx", "fxh"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "hlsl",
            .namePattern = "^hlsl$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_hlsl(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
