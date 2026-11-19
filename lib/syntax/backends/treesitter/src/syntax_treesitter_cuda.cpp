// The tree-sitter language of CUDA: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_cuda.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_cuda.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterCuda(void) {
    static const PrStringView kMediaTypes[] = {"text/x-cuda"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "cuda",
            .namePattern = "^(cuda|cu)$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_cuda(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
