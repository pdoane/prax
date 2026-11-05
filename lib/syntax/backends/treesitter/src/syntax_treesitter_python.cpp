// The tree-sitter language of Python: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_python.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_python.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterPython(void) {
    static const PrStringView kExtensions[] = {"py", "pyi"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "python",
            .namePattern = "^(py|python)$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_python(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
