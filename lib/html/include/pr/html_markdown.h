// Markdown in HTML: Markdown parsed into an element's children, whole or as it arrives.
//
// The children are what parsing the HTML that lib/markdown writes for the Markdown (pr/markdown_html.h) as the
// element's contents gives (html/#parsing-html-fragments). A browsing context loads a text/markdown resource as a
// document in the same way (pr/html_page.h).

#pragma once

#include "pr/base.h"
#include "pr/dom.h"
#include "pr/markdown.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Replaces the children of `element` with `markdown` parsed with `options`, or with the defaults if `options` is null.
// The parse runs no script, but raw HTML keeps what it is written with, including event handler content attributes. A
// host that shows Markdown it does not trust filters or escapes raw HTML (PrMarkdownOptions).
void prHtmlSetMarkdown(PrDomElement* element, PrStringView markdown, const PrMarkdownOptions* PR_NULLABLE options);

// Markdown whose source arrives in pieces, parsed into an element's children as it arrives. The children of the
// Markdown's final blocks stay as they are while more arrives, and the children after them are parsed again.
typedef struct PrHtmlMarkdownStream PrHtmlMarkdownStream;

// Creates a stream into `element`, whose children are removed, with `options` as prHtmlSetMarkdown takes them. The
// stream retains the element.
PrHtmlMarkdownStream* prHtmlMarkdownStreamCreate(PrDomElement* element, const PrMarkdownOptions* PR_NULLABLE options);

// Frees the stream. The element keeps the children that the stream parsed.
void prHtmlMarkdownStreamDestroy(PrHtmlMarkdownStream* PR_NULLABLE stream);

// Appends `text` to the source and updates the element's children, or ends the source. The source must be open. A piece
// may end anywhere, as prMarkdownAppend allows.
void prHtmlMarkdownStreamAppend(PrHtmlMarkdownStream* stream, PrStringView text);
void prHtmlMarkdownStreamEnd(PrHtmlMarkdownStream* stream);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
