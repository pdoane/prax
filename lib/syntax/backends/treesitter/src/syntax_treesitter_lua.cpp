// The tree-sitter language of Lua: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_lua.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_lua.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterLua(void) {
    static const PrStringView kExtensions[] = {"lua"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "lua",
            .namePattern = "^lua$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_lua(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
