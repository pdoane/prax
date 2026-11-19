// The tree-sitter language of Java: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_java.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_java.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterJava(void) {
    static const PrStringView kMediaTypes[] = {"text/x-java"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "java",
            .namePattern = "^java$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_java(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
