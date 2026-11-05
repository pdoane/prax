// The tree-sitter language of SQL: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_sql.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_sql.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterSql(void) {
    static const PrStringView kExtensions[] = {"sql"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "sql",
            .namePattern = "^sql$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .grammar = tree_sitter_sql(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
