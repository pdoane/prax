// The tree-sitter language of INI files: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_ini.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_ini.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterIni(void) {
    static const PrStringView kMediaTypes[] = {"text/x-ini"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "ini",
            .namePattern = "^(ini|dosini|cfg)$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_ini(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
