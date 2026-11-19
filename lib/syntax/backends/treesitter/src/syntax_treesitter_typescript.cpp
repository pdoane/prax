// The tree-sitter language of TypeScript: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_typescript.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_typescript.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterTypeScript(void) {
    static const PrStringView kMediaTypes[] = {"text/x-typescript"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "typescript",
            .namePattern = "^(ts|typescript)$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_typescript(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
