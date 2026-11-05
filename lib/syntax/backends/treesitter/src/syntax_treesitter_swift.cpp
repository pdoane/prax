// The tree-sitter language of Swift: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_swift.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_swift.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterSwift(void) {
    static const PrStringView kExtensions[] = {"swift"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "swift",
            .namePattern = "^swift$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_swift(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
