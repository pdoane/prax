// Markdown blocks: lines of source parsed into the block structure of a document (commonmark/#blocks-and-inlines).

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/markdown.h"
#include "pr/string_view.h"

#include "markdown/src/markdown_inline.h"
#include "markdown/src/markdown_tree.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A block of a parse, by its position in the parser's blocks. The root is block 0.
typedef uint32_t MarkdownBlockIndex;

// The index of no block.
constexpr MarkdownBlockIndex kMarkdownBlockNone = 0xffffffffu;

// A block as the parser builds it.
struct MarkdownParseBlock {
    PrMarkdownNodeKind kind;
    bool open;
    bool lastLineBlank;             // true if the block's last line was blank, which decides whether a list is tight
    bool lastLineChecked;           // true once a check for a blank last line has visited the block

    // Code blocks
    bool fenced;
    char fenceCharacter;
    uint8_t fenceLength;
    uint8_t fenceOffset;            // the indentation of the opening fence

    // Headings
    uint8_t level;
    bool setext;

    uint8_t htmlKind;               // HTML blocks: the start condition, 1 to 7

    // Tables and their rows and cells. A table's alignments are a range of the parser's alignments, by column.
    bool tableVisited;              // paragraphs: true once a delimiter row failed to make the paragraph a table
    bool header;                    // rows and cells: true for the table's first row
    PrMarkdownAlignment alignment;  // cells
    uint32_t columns;
    uint32_t rows;
    uint32_t filledCells;           // the cells that rows were missing and that the table added
    uint32_t alignmentStart;

    PrMarkdownTask task;            // items

    // Lists and items. An item holds the values of the list marker that starts it.
    PrMarkdownListKind list;
    char bulletCharacter;
    char delimiter;                 // an ordered list's '.' or ')'
    bool tight;
    uint32_t start;
    uint32_t markerOffset;          // the indentation of the item's marker
    uint32_t markerWidth;           // the columns from the marker's start to the item's content

    uint32_t startLine;             // the number of the block's first line in the parse
    uint32_t sourceStart;           // the offset in the source of the block's first line
    uint32_t lastLineSource;        // leaves: the offset in the source of the last line that the block took

    MarkdownBlockIndex parent;
    MarkdownBlockIndex first;
    MarkdownBlockIndex last;
    MarkdownBlockIndex previous;
    MarkdownBlockIndex next;

    // The block's text, as a range of the parser's content: a leaf's lines, each ending in '\n', or a table cell's
    // text.
    uint32_t contentStart;
    uint32_t contentEnd;
    MarkdownRange info;             // the info string of a fenced code block, or a footnote definition's label
};

// A link reference definition, as a parse finds it.
struct MarkdownParsedDefinition {
    uint32_t sourceStart;           // the offset in the source of its paragraph's first line
    MarkdownRange label;            // the normalized label, as a range of the parser's text
    MarkdownRange destination;
    MarkdownRange title;
};

// The state of a parse of blocks, kept between parses for its storage.
struct MarkdownBlockParser {
    Array<MarkdownParseBlock> blocks;   // by MarkdownBlockIndex
    Array<char> content;
    Array<char> line;
    Array<char> text;
    Array<MarkdownParsedDefinition> definitions;
    Array<PrMarkdownAlignment> alignments;

    // Where the final blocks of the last parse end: the offset in the source of the first tentative child of the root,
    // or after the last complete line if every child is final, and the number of children before that offset.
    uint32_t finalEnd;
    uint32_t finalCount;

    uint32_t rootChildren;          // the number of children of the root, kept as they are added and removed

    // The state of the line being parsed. A column counts tabs to the next tab stop, and an offset counts bytes.
    uint32_t lineNumber;
    uint32_t lineSource;            // the offset in the source of the line
    uint32_t offset;
    uint32_t column;
    uint32_t firstNonspace;
    uint32_t firstNonspaceColumn;
    uint32_t indent;
    uint32_t thematicBreakKillPosition;     // the offset before which no thematic break starts on this line
    bool blank;
    bool partiallyConsumedTab;
    MarkdownBlockIndex current;     // the deepest open block
};

// Frees the parser's storage.
void markdownBlockParserReset(MarkdownBlockParser* parser);

// Parses the lines of `source` from `start`, which is the start of a line, as the children of a new root. A line ends
// at a line ending. If `ended` is false, the source may continue, and a last line without a line ending is parsed as it
// is, but no block that it closes is final. Every block is closed once the parse ends, as if the source ended there.
void markdownParseBlocks(MarkdownBlockParser* parser, const PrMarkdownOptions& options, PrStringView source,
                         uint32_t start, bool ended);

// Builds the block `block` of the last parse, a child of the root, and its descendants into `tree` under its root, with
// their inlines parsed by `inlines`, whose references `resolve` looks up.
void markdownBuildBlock(MarkdownBlockParser* parser, const PrMarkdownOptions& options, MarkdownBlockIndex block,
                        MarkdownTree* tree, MarkdownInlineParser* inlines, MarkdownResolveFn resolve,
                        void* PR_NULLABLE context);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
