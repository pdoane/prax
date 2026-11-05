// The tree-sitter language of Objective-C: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_objc.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_objc.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterObjC(void) {
    static const PrStringView kExtensions[] = {"m", "mm"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "objc",
            .namePattern = "^(objc|objectivec|objective-c)$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_objc(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
