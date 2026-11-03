// The tree-sitter language of C++: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_cpp.h"

#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_cpp.h"

#include "tree-sitter-cpp.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterCpp(void) {
    static const PrStringView kExtensions[] = {"cpp", "cc", "cxx", "hpp", "hh", "hxx", "inl", "mm"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "cpp",
            .namePattern = "^(cc|cpp|c\\+\\+)$",
            .extensions = kExtensions,
            .extensionCount = 8,
            .grammar = tree_sitter_cpp(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
