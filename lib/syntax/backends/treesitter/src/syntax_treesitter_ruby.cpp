// The tree-sitter language of Ruby: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_ruby.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_ruby.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterRuby(void) {
    static const PrStringView kMediaTypes[] = {"text/x-ruby"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "ruby",
            .namePattern = "^(ruby|rb)$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_ruby(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
