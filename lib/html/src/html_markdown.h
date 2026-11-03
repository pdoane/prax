// Markdown in the DOM: a Markdown document's blocks built into a document or into an element.
//
// A builder feeds HTML tree construction the events of the HTML that lib/markdown writes for a Markdown document
// (pr/markdown_html.h): tags and characters as tokens that the builder makes itself, and raw HTML, which the tokenizer
// reads. The DOM is the one that parsing that HTML gives. While raw HTML leaves the tokenizer within a token or in a
// state other than data, such as the RCDATA of a textarea, the builder writes the HTML text of what follows for the
// tokenizer to read, as a parse of the whole HTML would.

#pragma once

#include "pr/dom.h"
#include "pr/markdown.h"
#include "pr/raw_array.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"

#include "html/src/html_parser.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The most nodes that a builder builds into: a document, its html element and its body.
constexpr uint32_t kHtmlMarkdownMaxTargets = 3;

// A point before a top-level block at which the parse is at rest (htmlParserIsAtRest) in the state that the builder
// started in. What the blocks before a clean point build does not depend on the blocks after it, so a change rebuilds
// from the last clean point before the change.
struct HtmlMarkdownCleanPoint {
    uint32_t block;                 // the top-level block that the point is before
    bool framesetOk;

    // The last child of each node that the blocks build into, which the point retains, or null if the node has none. A
    // later block can extend a last child that is a Text node, so its length is kept too.
    PrDomNode* PR_NULLABLE lastChildren[kHtmlMarkdownMaxTargets];
    uint32_t lastTextLengths[kHtmlMarkdownMaxTargets];
};

// A Markdown document built into a document or into an element.
struct HtmlMarkdownBuilder {
    PrMarkdown* markdown;
    HtmlParser parser;
    HtmlParseFlags flags;

    // The document, or the element, that the builder builds into. The builder retains it.
    PrDomDocument* PR_NULLABLE document;
    PrDomElement* PR_NULLABLE element;

    // The nodes that the blocks build into: the document, its html element and its body, or the element.
    PrDomNode* PR_NULLABLE targets[kHtmlMarkdownMaxTargets];
    uint32_t targetCount;

    HtmlParserRest base;            // the state of the parse at a clean point, apart from its frameset-ok flag

    RawArray<HtmlMarkdownCleanPoint> cleanPoints; // in the order of their blocks
    uint32_t built;                               // the number of top-level blocks built

    PrRawString html;                  // the HTML text of an event that the tokenizer reads
};

// Starts building into `document`, which must be empty, as an HTML document whose body holds the blocks. The parse runs
// with `flags`, and never runs script. The builder retains the document.
void htmlMarkdownBuilderInitDocument(HtmlMarkdownBuilder* builder, PrDomDocument* document, HtmlParseFlags flags,
    const PrMarkdownOptions* PR_NULLABLE options);

// Starts building into `element`, whose children are removed. The blocks are parsed as the element's contents
// (html/#parsing-html-fragments), and the parse never runs script. The builder retains the element.
void htmlMarkdownBuilderInitElement(HtmlMarkdownBuilder* builder, PrDomElement* element,
    const PrMarkdownOptions* PR_NULLABLE options);

// Appends `text` to the Markdown source and builds the blocks that changed, or ends the source and builds the rest.
void htmlMarkdownBuilderAppend(HtmlMarkdownBuilder* builder, PrStringView text);
void htmlMarkdownBuilderEnd(HtmlMarkdownBuilder* builder);

// Frees what the builder owns and releases what it retains. The DOM that it built stays.
void htmlMarkdownBuilderReset(HtmlMarkdownBuilder* builder);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
