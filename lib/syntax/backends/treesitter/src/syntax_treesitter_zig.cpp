// The tree-sitter language of Zig: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_zig.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_zig.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterZig(void) {
    static const PrStringView kMediaTypes[] = {"text/x-zig"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "zig",
            .namePattern = "^zig$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_zig(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
