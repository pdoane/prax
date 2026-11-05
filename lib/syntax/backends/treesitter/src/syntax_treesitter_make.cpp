// The tree-sitter language of a makefile: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_make.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_make.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterMake(void) {
    static const PrStringView kExtensions[] = {"mk", "mak", "make"};
    static const PrStringView kFileNames[] = {"Makefile", "makefile", "GNUmakefile"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "make",
            .namePattern = "^(make|makefile|mk)$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .fileNames = kFileNames,
            .fileNameCount = countOf(kFileNames),
            .grammar = tree_sitter_make(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
