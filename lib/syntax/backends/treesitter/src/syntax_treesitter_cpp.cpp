// The tree-sitter language of C++: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_cpp.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_cpp.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterCpp(void) {
    static const PrStringView kExtensions[] =
        {"cpp", "cc", "cxx", "c++", "h", "hpp", "hh", "hxx", "h++", "inl", "ipp", "tpp", "cppm", "ixx"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "cpp",
            .namePattern = "^(cc|cpp|cxx|c\\+\\+|h|hpp)$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_cpp(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
