// The tree-sitter language of Starlark, the language of Bazel's build files: its grammar, and the queries that the
// build reads from its package.

#include "pr/syntax_treesitter_starlark.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_starlark.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterStarlark(void) {
    static const PrStringView kMediaTypes[] = {"text/x-starlark"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "starlark",
            .namePattern = "^(starlark|bazel|bzl)$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_starlark(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
