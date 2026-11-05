// The tree-sitter language of JSON: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_json.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_json.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterJson(void) {
    static const PrStringView kExtensions[] = {"json"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "json",
            .namePattern = "^json$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_json(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
