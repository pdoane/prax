// The tree-sitter language of GLSL: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_glsl.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_glsl.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterGlsl(void) {
    static const PrStringView kMediaTypes[] = {"text/x-glsl"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "glsl",
            .namePattern = "^glsl$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_glsl(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
