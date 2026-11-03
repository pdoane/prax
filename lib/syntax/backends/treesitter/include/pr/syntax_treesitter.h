// Languages over tree-sitter grammars (https://tree-sitter.github.io), whose trees each update parses again
// incrementally from the edit stream.
//
// A language reads a grammar's highlights query, whose names are the categories of its spans, and its injections query,
// whose matches hand ranges to other languages of the registry. An injected range is parsed afresh when spans of it are
// asked for, so its cost follows what is shown. Each grammar that the backend builds in has a header of its own
// (pr/syntax_treesitter_<language>.h), so a program links only the grammars that it names.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"
#include "pr/syntax.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A tree-sitter grammar and its queries, in the forms that tree-sitter's own tools read.
typedef struct PrSyntaxTreeSitterDesc {
    PrStringView name;
    PrStringView namePattern;       // as PrSyntaxLanguage::namePattern, the grammar's injection-regex
    const PrStringView* PR_NULLABLE extensions;
    uint32_t extensionCount;
    const PrStringView* PR_NULLABLE fileNames;
    uint32_t fileNameCount;

    // The grammar's TSLanguage, which must outlive the language, of an ABI that the built tree-sitter reads.
    const void* grammar;

    // The text of the grammar's queries (queries/highlights.scm and queries/injections.scm). A query that does not
    // compile leaves the language without what it gives.
    PrStringView highlights;
    PrStringView injections;
} PrSyntaxTreeSitterDesc;

// Creates a language of `desc`, or frees one. The language copies what it keeps of `desc`.
const PrSyntaxLanguage* prSyntaxTreeSitterLanguageCreate(const PrSyntaxTreeSitterDesc* desc);
void prSyntaxTreeSitterLanguageDestroy(const PrSyntaxLanguage* PR_NULLABLE language);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
