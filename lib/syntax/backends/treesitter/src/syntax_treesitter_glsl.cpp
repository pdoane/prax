// The tree-sitter language of GLSL: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_glsl.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_glsl.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterGlsl(void) {
    static const PrStringView kExtensions[] =
        {"glsl", "vert", "frag", "geom", "comp", "tesc", "tese", "mesh", "task", "rgen", "rchit", "rmiss", "vsh", "fsh"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "glsl",
            .namePattern = "^glsl$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_glsl(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
