// The tree-sitter language of YAML: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_yaml.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_yaml.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterYaml(void) {
    static const PrStringView kMediaTypes[] = {"application/yaml", "+yaml"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "yaml",
            .namePattern = "^(yaml|yml)$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_yaml(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
