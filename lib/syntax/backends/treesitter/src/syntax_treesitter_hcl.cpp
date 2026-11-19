// The tree-sitter language of HCL: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_hcl.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_hcl.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterHcl(void) {
    static const PrStringView kMediaTypes[] = {"text/x-hcl"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "hcl",
            .namePattern = "^hcl$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_hcl(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
