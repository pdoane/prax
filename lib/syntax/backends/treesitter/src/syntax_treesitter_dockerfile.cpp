// The tree-sitter language of a Dockerfile: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_dockerfile.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_dockerfile.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterDockerfile(void) {
    static const PrStringView kExtensions[] = {"dockerfile", "containerfile"};
    static const PrStringView kFileNames[] = {"Dockerfile", "Containerfile"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "dockerfile",
            .namePattern = "^(dockerfile|docker|containerfile)$",
            .extensions = kExtensions,
            .extensionCount = countOf(kExtensions),
            .fileNames = kFileNames,
            .fileNameCount = countOf(kFileNames),
            .grammar = tree_sitter_dockerfile(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
