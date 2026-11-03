// Markdown inlines: the content of a paragraph or a heading, parsed into inline nodes (commonmark/#inlines).

#pragma once

#include "pr/base.h"
#include "pr/markdown.h"
#include "pr/raw_array.h"
#include "pr/string_view.h"

#include "markdown/src/markdown_tree.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The destination and the title that a link reference definition gives its label.
struct MarkdownDefinition {
    PrStringView destination;
    PrStringView title;
};

// Called with the normalized label of each reference that the inlines look up. Writes the definition to `out` and
// returns true if one matches the label.
typedef bool (*MarkdownResolveFn)(void* PR_NULLABLE context, PrStringView label, MarkdownDefinition* out);

// A delimiter run that may open or close emphasis, a strikethrough, a highlight, a subscript, a superscript or a pair
// of quotes (commonmark/#delimiter-run).
struct MarkdownDelimiter {
    MarkdownNodeIndex node;         // the Text node of the run's characters that are not yet used
    uint32_t previous;              // the delimiter before this one, by position in the parser's delimiters
    uint32_t next;                  // the delimiter after this one
    uint32_t position;              // the offset in the content after the run
    uint32_t length;                // the length of the whole run, which the rule of 3 reads
    char character;
    bool canOpen;
    bool canClose;
};

// A '[' or "![" that may open a link or an image.
struct MarkdownBracket {
    MarkdownNodeIndex node;         // the Text node of the bracket's characters
    uint32_t position;              // the offset in the content after the bracket
    bool image;
    bool bracketAfter;              // true if another bracket opened after this one
};

// A number of backticks in a backtick string, which indexes the parser's backticks.
typedef uint32_t MarkdownBacktickCount;

// The state of parsing one block's inlines, kept between blocks for its storage.
struct MarkdownInlineParser {
    RawArray<MarkdownDelimiter> delimiters;
    RawArray<MarkdownBracket> brackets;
    RawArray<char> label;
    RawArray<uint32_t> backticks;      // by MarkdownBacktickCount: where the last string of that length starts
};

// Frees the parser's storage.
void markdownInlineParserReset(MarkdownInlineParser* parser);

// Parses `content`, the text of a paragraph or a heading, and appends its inlines to `parent` in `tree`. Lines in the
// content end with '\n'. `resolve` looks up the references in the content.
void markdownParseInlines(MarkdownInlineParser* parser, const PrMarkdownOptions& options, MarkdownTree* tree,
    MarkdownNodeIndex parent, PrStringView content, MarkdownResolveFn resolve, void* PR_NULLABLE context);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
