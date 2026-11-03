// Markdown: CommonMark and its extensions, parsed into a document that a client walks.

#pragma once

#include "pr/base.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Options

// The extensions to CommonMark that a document parses.
enum {
    PrMarkdownExtensionFlag_Tables = 1u << 0,           // GFM tables
    PrMarkdownExtensionFlag_TaskLists = 1u << 1,        // GFM task list items
    PrMarkdownExtensionFlag_Strikethrough = 1u << 2,    // GFM: text between one or two tildes
    PrMarkdownExtensionFlag_Autolinks = 1u << 3,        // GFM: URLs and email addresses without angle brackets
    PrMarkdownExtensionFlag_Footnotes = 1u << 4,        // GitHub's footnotes

    // Curly quotes for straight ones, dashes for runs of two or more hyphens, and an ellipsis for "..."
    PrMarkdownExtensionFlag_SmartPunctuation = 1u << 5,

    PrMarkdownExtensionFlag_Alerts = 1u << 6,           // GitHub's alerts: a block quote whose first line is [!NOTE]
    PrMarkdownExtensionFlag_HeadingIds = 1u << 7,       // an id for each heading, as GitHub makes it from the text
    PrMarkdownExtensionFlag_Math = 1u << 8,             // GitHub's math: TeX between $, between $$, or in $`…`$
    PrMarkdownExtensionFlag_Highlight = 1u << 9,        // text between two equals signs on each side

    // pandoc's subscripts between tildes and superscripts between carets. With them, one tilde is a subscript and two
    // are a strikethrough.
    PrMarkdownExtensionFlag_Scripts = 1u << 10,

    // A leading block between "---" lines, which the document holds as it is written, and does not parse. While a
    // source that starts with "---" is open and has no closing line, no block of it is final.
    PrMarkdownExtensionFlag_FrontMatter = 1u << 11,

    PrMarkdownExtensionFlag_Gfm = PrMarkdownExtensionFlag_Tables | PrMarkdownExtensionFlag_TaskLists |
        PrMarkdownExtensionFlag_Strikethrough | PrMarkdownExtensionFlag_Autolinks,

    // What GitHub renders: GFM, footnotes, alerts, heading ids and math
    PrMarkdownExtensionFlag_GitHub = PrMarkdownExtensionFlag_Gfm | PrMarkdownExtensionFlag_Footnotes |
        PrMarkdownExtensionFlag_Alerts | PrMarkdownExtensionFlag_HeadingIds | PrMarkdownExtensionFlag_Math,
};
typedef uint32_t PrMarkdownExtensionFlags;

// What raw HTML in the source becomes.
enum {
    PrMarkdownRawHtml_Pass,         // HTML, as it is written
    PrMarkdownRawHtml_Filter,       // HTML, with GFM's disallowed tags escaped so that they read as text
    PrMarkdownRawHtml_Escape,       // text
};
typedef uint8_t PrMarkdownRawHtml;

// Which destinations of links and images a document keeps.
enum {
    PrMarkdownDestinations_All,

    // Every destination except those whose scheme can run code or read local files: javascript:, vbscript:, file:, and
    // data: other than an image's. A destination that is dropped is empty.
    PrMarkdownDestinations_Safe,
};
typedef uint8_t PrMarkdownDestinations;

// How a document parses its source.
typedef struct PrMarkdownOptions {
    PrMarkdownExtensionFlags extensions;
    PrMarkdownRawHtml rawHtml;
    PrMarkdownDestinations destinations;
} PrMarkdownOptions;

// The extensions that GitHub renders, with raw HTML passed through and every destination kept.
PrMarkdownOptions prMarkdownOptionsDefault(void);

// ---------------------------------------------------------------------------------------------------------------------
// Documents

// The blocks parsed from Markdown source. A document's source arrives whole or in pieces, and the document is read by
// walking its top-level blocks (prMarkdownWalk).
//
// Each top-level block is either final or tentative. A final block never changes, except that a link reference
// definition later in the source can resolve a reference that the block failed to resolve. The blocks from the first
// tentative one onward are parsed again as more source arrives. Once the source ends, every block is final, and the
// document is the same as if its source had been parsed whole.
typedef struct PrMarkdown PrMarkdown;

// Creates an empty document whose source is open. A null `options` means the defaults.
PrMarkdown* prMarkdownCreate(const PrMarkdownOptions* PR_NULLABLE options);

// Frees the document.
void prMarkdownDestroy(PrMarkdown* PR_NULLABLE document);

// Empties the document and opens its source again, keeping its storage. The changes not yet taken are discarded, so a
// client that keeps state from earlier walks starts over.
void prMarkdownClear(PrMarkdown* document);

// Appends `text` to the source, or ends the source. The source must be open. Text is UTF-8, and a piece may end
// anywhere, including inside a code point or between the CR and LF of a line ending. A last line without its line
// ending is parsed as it arrives if it is text, and waits for its line ending if the rest of the line can still change
// what it shows, as "[" may start a link reference definition or "--" a setext underline.
void prMarkdownAppend(PrMarkdown* document, PrStringView text);
void prMarkdownEnd(PrMarkdown* document);

// Replaces the document with `text` parsed whole, and ends its source.
void prMarkdownParse(PrMarkdown* document, PrStringView text);

// Returns true if the source has ended.
bool prMarkdownIsEnded(const PrMarkdown* document);

// Returns the number of top-level blocks.
uint32_t prMarkdownBlockCount(const PrMarkdown* document);

// Returns the number of top-level blocks that are final. They are the first blocks of the document.
uint32_t prMarkdownFinalCount(const PrMarkdown* document);

// ---------------------------------------------------------------------------------------------------------------------
// Changes

// A change to the top-level blocks: the `removedCount` blocks at `first` were replaced by `insertedCount` blocks.
typedef struct PrMarkdownChange {
    uint32_t first;
    uint32_t removedCount;
    uint32_t insertedCount;
} PrMarkdownChange;

// Writes up to `capacity` of the changes since the last call to `out`, and returns how many it wrote. A client calls it
// until it writes none. Applying the changes in order to the blocks that the client last read gives the document's
// blocks. A block whose late reference resolved is reported as one block replaced by one.
uint32_t prMarkdownTakeChanges(PrMarkdown* document, PrMarkdownChange* PR_NULLABLE out, uint32_t capacity);

// Returns true if the footnotes that prMarkdownWalkFootnotes walks changed since the last call.
bool prMarkdownTakeFootnotesChanged(PrMarkdown* document);

// ---------------------------------------------------------------------------------------------------------------------
// Walks

// The kinds of node.
enum {
    // Blocks
    PrMarkdownNodeKind_Paragraph,
    PrMarkdownNodeKind_Heading,
    PrMarkdownNodeKind_ThematicBreak,
    PrMarkdownNodeKind_BlockQuote,
    PrMarkdownNodeKind_List,
    PrMarkdownNodeKind_Item,
    PrMarkdownNodeKind_CodeBlock,
    PrMarkdownNodeKind_HtmlBlock,
    PrMarkdownNodeKind_Table,
    PrMarkdownNodeKind_TableHead,
    PrMarkdownNodeKind_TableBody,
    PrMarkdownNodeKind_TableRow,
    PrMarkdownNodeKind_TableCell,
    PrMarkdownNodeKind_FootnoteDefinition,
    PrMarkdownNodeKind_Alert,
    PrMarkdownNodeKind_FrontMatter,

    // Inlines
    PrMarkdownNodeKind_Text,
    PrMarkdownNodeKind_SoftBreak,
    PrMarkdownNodeKind_HardBreak,
    PrMarkdownNodeKind_Code,
    PrMarkdownNodeKind_Emphasis,
    PrMarkdownNodeKind_Strong,
    PrMarkdownNodeKind_Strikethrough,
    PrMarkdownNodeKind_Link,
    PrMarkdownNodeKind_Image,
    PrMarkdownNodeKind_FootnoteReference,
    PrMarkdownNodeKind_Highlight,
    PrMarkdownNodeKind_Subscript,
    PrMarkdownNodeKind_Superscript,
    PrMarkdownNodeKind_Math,
};
typedef uint8_t PrMarkdownNodeKind;

// The steps of a walk. A node is entered, then its children are walked, then it is left. A Text node is one Text step
// instead.
enum {
    PrMarkdownStepKind_Enter,
    PrMarkdownStepKind_Leave,
    PrMarkdownStepKind_Text,
};
typedef uint8_t PrMarkdownStepKind;

// What the characters of a Text step are.
enum {
    PrMarkdownTextKind_Plain,           // characters, with escapes and character references decoded
    PrMarkdownTextKind_Html,            // raw HTML, as it is written
};
typedef uint8_t PrMarkdownTextKind;

// The kinds of list.
enum {
    PrMarkdownListKind_Bullet,
    PrMarkdownListKind_Ordered,
};
typedef uint8_t PrMarkdownListKind;

// The state of a task list item's checkbox.
enum {
    PrMarkdownTask_None,            // the item is not a task
    PrMarkdownTask_Unchecked,
    PrMarkdownTask_Checked,
};
typedef uint8_t PrMarkdownTask;

// The kinds of alert, which GitHub writes as [!NOTE] and the like.
enum {
    PrMarkdownAlert_None,
    PrMarkdownAlert_Note,
    PrMarkdownAlert_Tip,
    PrMarkdownAlert_Important,
    PrMarkdownAlert_Warning,
    PrMarkdownAlert_Caution,
};
typedef uint8_t PrMarkdownAlert;

// The alignment of a table column.
enum {
    PrMarkdownAlignment_None,
    PrMarkdownAlignment_Left,
    PrMarkdownAlignment_Center,
    PrMarkdownAlignment_Right,
};
typedef uint8_t PrMarkdownAlignment;

// One step of a walk. Each field is set only for the steps of the nodes that the comment names, and is zero otherwise.
// The text of each view is valid until the document changes.
typedef struct PrMarkdownStep {
    PrMarkdownStepKind kind;
    PrMarkdownNodeKind node;
    PrMarkdownTextKind textKind;    // Text
    uint8_t level;                  // Heading: 1 to 6
    PrMarkdownListKind list;        // List

    // List: true if the list is tight. The paragraphs of a tight list's items are not walked as nodes, so their inlines
    // are children of the item.
    bool tight;

    PrMarkdownTask task;            // Item
    PrMarkdownAlert alert;          // Alert
    bool display;                   // Math: true for display math, between $$, and false for inline math
    PrMarkdownAlignment alignment;  // TableCell
    bool header;                    // TableCell: true if the cell is in the table's head
    uint32_t start;                 // List: the number of an ordered list's first item
    PrStringView text;              // Text
    PrStringView id;                // Heading, with heading ids: the heading's id, which may be empty
    PrStringView info;              // CodeBlock: the info string of a fenced block, which is empty for any other

    // Link, Image: the destination, and the title. The destination is not percent-encoded. A link's children are its
    // text, and an image's children are its description.
    PrStringView destination;
    PrStringView title;

    // FootnoteDefinition, FootnoteReference: the definition's label as it is written, and the footnote's number, which
    // counts footnotes in the order that they are first referenced.
    PrStringView label;
    uint32_t number;

    uint32_t reference;             // FootnoteReference: which reference to its footnote this is, from 1
    uint32_t referenceCount;        // FootnoteDefinition: the number of references to the footnote
} PrMarkdownStep;

// Called with each step of a walk, in document order. The step is valid only during the call.
typedef void (*PrMarkdownStepFn)(void* PR_NULLABLE context, const PrMarkdownStep* step);

// Walks the `count` top-level blocks at `first`, which must be blocks of the document, and calls `fn` with each step.
// Footnote definitions are not walked with the blocks. A reference to a footnote that no definition has is walked as
// the Text that it is written as.
void prMarkdownWalk(const PrMarkdown* document, uint32_t first, uint32_t count, PrMarkdownStepFn fn, void* PR_NULLABLE context);

// Walks the definitions of the footnotes that the document references, in the order of their numbers, and calls `fn`
// with each step.
void prMarkdownWalkFootnotes(const PrMarkdown* document, PrMarkdownStepFn fn, void* PR_NULLABLE context);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
