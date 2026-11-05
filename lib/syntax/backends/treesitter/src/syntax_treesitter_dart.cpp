// The tree-sitter language of Dart: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_dart.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_dart.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterDart(void) {
    static const PrStringView kExtensions[] = {"dart"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "dart",
            .namePattern = "^dart$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_dart(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
