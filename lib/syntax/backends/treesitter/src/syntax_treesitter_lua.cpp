// The tree-sitter language of Lua: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_lua.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_lua.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterLua(void) {
    static const PrStringView kMediaTypes[] = {"text/x-lua"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "lua",
            .namePattern = "^lua$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_lua(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
