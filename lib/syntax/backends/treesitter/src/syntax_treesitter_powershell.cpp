// The tree-sitter language of PowerShell: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_powershell.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_powershell.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterPowerShell(void) {
    static const PrStringView kMediaTypes[] = {"text/x-powershell"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "powershell",
            .namePattern = "^(powershell|pwsh|ps1)$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_powershell(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
