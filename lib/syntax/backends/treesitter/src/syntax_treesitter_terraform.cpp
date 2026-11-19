// The tree-sitter language of Terraform: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_terraform.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_terraform.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterTerraform(void) {
    static const PrStringView kMediaTypes[] = {"text/x-terraform"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "terraform",
            .namePattern = "^(terraform|tf)$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_terraform(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
