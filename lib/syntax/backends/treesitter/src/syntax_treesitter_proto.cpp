// The tree-sitter language of Protocol Buffers: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_proto.h"

#include "pr/count_of.h"
#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_proto.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterProto(void) {
    static const PrStringView kMediaTypes[] = {"text/x-protobuf"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "proto",
            .namePattern = "^(proto|protobuf)$",
            .mediaTypes = kMediaTypes,
            .mediaTypeCount = countOf(kMediaTypes),
            .grammar = tree_sitter_proto(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
