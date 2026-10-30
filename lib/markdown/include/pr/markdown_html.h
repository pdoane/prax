// Markdown as HTML: the tags and text of a document's HTML, and the HTML text that they make.

#pragma once

#include "pr/base.h"
#include "pr/markdown.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Events

// The kinds of HTML event.
enum {
    PrMarkdownHtmlEventKind_StartTag,
    PrMarkdownHtmlEventKind_EndTag,
    PrMarkdownHtmlEventKind_Text,       // characters
    PrMarkdownHtmlEventKind_Html,       // raw HTML, as it is written
};
typedef uint8_t PrMarkdownHtmlEventKind;

// An attribute of a start tag. A value that is the null view is an attribute written without a value, as
// `data-footnote-ref` is, and differs from the empty value of `checked=""`.
typedef struct PrMarkdownHtmlAttribute {
    PrStringView name;
    PrStringView value;
} PrMarkdownHtmlAttribute;

// One event of a document's HTML. Text and values are not escaped. The URLs of href and src values, and the labels in
// the ids of footnotes, are percent-encoded, as prMarkdownEncodeUrl writes them.
typedef struct PrMarkdownHtmlEvent {
    PrMarkdownHtmlEventKind kind;
    bool selfClosing;               // StartTag: true for an element written as `<name />`, which has no end tag
    PrStringView name;              // StartTag, EndTag
    PrStringView text;              // Text, Html

    // StartTag: the attributes in the order that they are written.
    const PrMarkdownHtmlAttribute* PR_NULLABLE attributes;
    uint32_t attributeCount;
} PrMarkdownHtmlEvent;

// Called with each event of a document's HTML, in order. The event and its views are valid only during the call.
typedef void (*PrMarkdownHtmlFn)(void* PR_NULLABLE context, const PrMarkdownHtmlEvent* event);

// Calls `fn` with the events of the HTML of the `count` top-level blocks at `first`, which must be blocks of the
// document. The HTML is what the CommonMark specification's examples give, with the line feeds between blocks as Text.
// Raw HTML is as the document's options leave it.
void prMarkdownWriteHtml(const PrMarkdown* document, uint32_t first, uint32_t count,
    PrMarkdownHtmlFn fn, void* PR_NULLABLE context);

// Calls `fn` with the events of the HTML of the document's footnotes: a section that lists them, as GitHub writes it,
// or nothing if the document references no footnote. The HTML of a whole document is its blocks' followed by its
// footnotes'.
void prMarkdownWriteFootnotesHtml(const PrMarkdown* document, PrMarkdownHtmlFn fn, void* PR_NULLABLE context);

// Appends the HTML text of `event` to `out`, with its text and values escaped, as prMarkdownAppendHtml writes each
// event.
void prMarkdownHtmlEventAppendText(const PrMarkdownHtmlEvent* event, PrString* out);

// Appends `url` to `out` percent-encoded. A letter, a digit, a '%' and the punctuation that a URL holds as it is stay
// as they are, and every other byte is written as %XX. The characters '&' and '\'' are kept, and an HTML writer escapes
// them.
void prMarkdownEncodeUrl(PrStringView url, PrString* out);

// ---------------------------------------------------------------------------------------------------------------------
// HTML text

// Appends the HTML text of the `count` top-level blocks at `first`, or of the footnotes, to `out`. The text is the
// events written out, with text and values escaped.
void prMarkdownAppendHtml(const PrMarkdown* document, uint32_t first, uint32_t count, PrString* out);
void prMarkdownAppendFootnotesHtml(const PrMarkdown* document, PrString* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
