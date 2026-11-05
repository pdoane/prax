// The tree-sitter language of CMake: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_cmake.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_cmake.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterCMake(void) {
    static const PrStringView kExtensions[] = {"cmake"};
    static const PrStringView kFileNames[] = {"CMakeLists.txt"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "cmake",
            .namePattern = "^cmake$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .fileNames = kFileNames,
            .fileNameCount = countOf(kFileNames),
            .grammar = tree_sitter_cmake(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
