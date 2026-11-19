// The tree-sitter language of Bash: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_bash.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_bash.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterBash(void) {
    static const PrStringView kMediaTypes[] = {"text/x-shellscript", "application/x-sh"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "bash",
            .namePattern = "^(bash|sh|shell|zsh)$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_bash(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
