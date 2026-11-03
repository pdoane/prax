// Markdown blocks: lines of source parsed into the block structure of a document.

#include "markdown/src/markdown_block.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/string_ops.h"
#include "pr/string_trim.h"
#include "pr/unicode.h"
#include "pr/utf8.h"

#include "markdown/src/markdown_scan.h"

namespace pr_impl {

namespace {

// The kind of the root, which stands for the document. It is not a node kind.
constexpr PrMarkdownNodeKind kDocumentKind = 0xff;

// The width of a tab stop, and the indentation that starts an indented code block.
constexpr uint32_t kTabStop = 4;
constexpr uint32_t kCodeIndent = 4;

// The most blocks that a line may have opened before it opens a list item or a footnote definition. The limit bounds
// the depth of the lists that one line starts.
constexpr uint32_t kMaxOpenedPerLine = 100;

// The most digits in an ordered list item's number (commonmark/#ordered-list-marker).
constexpr uint32_t kMaxListDigits = 9;

// The most cells that a table adds to rows that are missing them. The limit keeps a table of many short rows from
// growing quadratically (gfm/#tables-extension).
constexpr uint32_t kMaxFilledCells = 0x80000;

MarkdownParseBlock& blockOf(MarkdownBlockParser* p, MarkdownBlockIndex block) {
    return p->blocks.data[block];
}

char peek(const MarkdownBlockParser* p, uint32_t index) {
    return index < p->line.count ? p->line.data[index] : '\0';
}

bool isLineEnd(char c) {
    return c == '\n' || c == '\0';
}

PrStringView lineView(const MarkdownBlockParser* p) {
    return PrStringView{p->line.data, p->line.count};
}

bool acceptsLines(PrMarkdownNodeKind kind) {
    return kind == PrMarkdownNodeKind_Paragraph || kind == PrMarkdownNodeKind_Heading ||
        kind == PrMarkdownNodeKind_CodeBlock;
}

bool canContain(PrMarkdownNodeKind parent, PrMarkdownNodeKind child) {
    switch (parent) {
        case kDocumentKind:
        case PrMarkdownNodeKind_BlockQuote:
        case PrMarkdownNodeKind_Item:
        case PrMarkdownNodeKind_FootnoteDefinition:
            return child != PrMarkdownNodeKind_Item;
        case PrMarkdownNodeKind_List:
            return child == PrMarkdownNodeKind_Item;
        case PrMarkdownNodeKind_Table:
            return child == PrMarkdownNodeKind_TableRow;
        case PrMarkdownNodeKind_TableRow:
            return child == PrMarkdownNodeKind_TableCell;
        default:
            return false;
    }
}

MarkdownBlockIndex createBlock(MarkdownBlockParser* p, PrMarkdownNodeKind kind) {
    MarkdownBlockIndex index = p->blocks.count;
    MarkdownParseBlock& block = p->blocks.emplace();
    block = MarkdownParseBlock{};
    block.kind = kind;
    block.open = true;
    block.startLine = p->lineNumber;
    block.sourceStart = p->lineSource;
    block.parent = kMarkdownBlockNone;
    block.first = kMarkdownBlockNone;
    block.last = kMarkdownBlockNone;
    block.previous = kMarkdownBlockNone;
    block.next = kMarkdownBlockNone;
    block.contentStart = p->content.count;
    block.contentEnd = p->content.count;
    return index;
}

void unlinkBlock(MarkdownBlockParser* p, MarkdownBlockIndex index) {
    MarkdownParseBlock& block = blockOf(p, index);
    if (block.parent == 0)
        --p->rootChildren;
    MarkdownParseBlock& parent = blockOf(p, block.parent);
    if (block.previous != kMarkdownBlockNone)
        blockOf(p, block.previous).next = block.next;
    else
        parent.first = block.next;
    if (block.next != kMarkdownBlockNone)
        blockOf(p, block.next).previous = block.previous;
    else
        parent.last = block.previous;
    block.parent = kMarkdownBlockNone;
}

// ---------------------------------------------------------------------------------------------------------------------
// Link reference definitions

void skipSpaces(PrStringView text, size_t* index) {
    while (*index < text.size && prAsciiIsTabOrSpace(text.data[*index]))
        ++*index;
}

// Moves past a line ending at `*index`, or returns false if there is none and the text does not end there.
bool skipLineEnd(PrStringView text, size_t* index) {
    if (*index < text.size && text.data[*index] == '\n') {
        ++*index;
        return true;
    }
    return *index >= text.size;
}

// Parses the link reference definition at the start of `text` and records it. Returns its length, or 0 if the text does
// not start with one (commonmark/#link-reference-definitions).
size_t parseDefinition(MarkdownBlockParser* p, PrStringView text, uint32_t sourceStart) {
    if (text.size == 0 || text.data[0] != '[')
        return 0;
    size_t index = 1;
    while (index < text.size && text.data[index] != '[' && text.data[index] != ']') {
        if (text.data[index] == '\\' && index + 1 < text.size && markdownIsAsciiPunctuation(text.data[index + 1]))
            ++index;
        ++index;
        if (index - 1 > kMarkdownMaxLabelBytes)
            return 0;
    }
    if (index >= text.size || text.data[index] != ']')
        return 0;
    PrStringView label = prStringSlice(text, 1, index - 1);
    ++index;
    if (index >= text.size || text.data[index] != ':')
        return 0;
    index = markdownSkipSpaceAndLineEnding(text, index + 1);

    size_t destinationLength = 0;
    MarkdownSpan destination{};
    if (!markdownScanLinkDestination(text, index, &destinationLength, &destination))
        return 0;
    // An empty destination needs angle brackets.
    if (destinationLength == 0)
        return 0;
    index += destinationLength;

    size_t beforeTitle = index;
    index = markdownSkipSpaceAndLineEnding(text, index);
    MarkdownSpan title{};
    size_t titleLength = index == beforeTitle ? 0 : markdownScanLinkTitle(text, index, &title);
    if (titleLength != 0)
        index += titleLength;
    else
        index = beforeTitle;
    skipSpaces(text, &index);
    if (!skipLineEnd(text, &index)) {
        if (titleLength == 0)
            return 0;
        // The title may instead be text that follows the definition.
        titleLength = 0;
        index = beforeTitle;
        skipSpaces(text, &index);
        if (!skipLineEnd(text, &index))
            return 0;
    }

    uint32_t labelStart = p->text.count;
    if (!markdownAppendNormalizedLabel(label, &p->text)) {
        p->text.resize(labelStart);
        return 0;
    }
    MarkdownParsedDefinition& definition = p->definitions.emplace();
    definition.sourceStart = sourceStart;
    definition.label = MarkdownRange{labelStart, p->text.count - labelStart};
    uint32_t mark = p->text.count;
    markdownAppendDecoded(prStringSlice(text, destination.start, destination.size), &p->text);
    definition.destination = MarkdownRange{mark, p->text.count - mark};
    mark = p->text.count;
    if (titleLength != 0)
        markdownAppendDecoded(prStringSlice(text, title.start, title.size), &p->text);
    definition.title = MarkdownRange{mark, p->text.count - mark};
    return index;
}

// Records the link reference definitions at the start of a paragraph and removes them from its content. Returns true if
// content remains that is not blank.
bool resolveDefinitions(MarkdownBlockParser* p, MarkdownBlockIndex index) {
    MarkdownParseBlock& block = blockOf(p, index);
    while (block.contentStart < block.contentEnd) {
        PrStringView text{p->content.data + block.contentStart, size_t(block.contentEnd - block.contentStart)};
        size_t length = parseDefinition(p, text, block.sourceStart);
        if (length == 0)
            break;
        block.contentStart += uint32_t(length);
    }
    for (uint32_t i = block.contentStart; i < block.contentEnd; ++i) {
        char c = p->content.data[i];
        if (!prAsciiIsTabOrSpace(c) && c != '\n')
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Finalizing

bool endsWithBlankLine(MarkdownBlockParser* p, MarkdownBlockIndex index) {
    while (true) {
        MarkdownParseBlock& block = blockOf(p, index);
        if (block.lastLineChecked)
            return block.lastLineBlank;
        block.lastLineChecked = true;
        bool descends = (block.kind == PrMarkdownNodeKind_List || block.kind == PrMarkdownNodeKind_Item) &&
            block.last != kMarkdownBlockNone;
        if (!descends)
            return block.lastLineBlank;
        index = block.last;
    }
}

// Closes `index` and returns its parent.
MarkdownBlockIndex finalize(MarkdownBlockParser* p, MarkdownBlockIndex index) {
    MarkdownParseBlock& block = blockOf(p, index);
    PR_ASSERT(block.open);
    block.open = false;
    MarkdownBlockIndex parent = block.parent;
    switch (block.kind) {
        case PrMarkdownNodeKind_Paragraph:
            if (!resolveDefinitions(p, index))
                unlinkBlock(p, index);
            break;
        case PrMarkdownNodeKind_CodeBlock:
            if (!block.fenced) {
                // Trailing blank lines are not part of an indented code block.
                uint32_t end = block.contentEnd;
                while (end > block.contentStart) {
                    char c = p->content.data[end - 1];
                    if (!prAsciiIsTabOrSpace(c) && c != '\n')
                        break;
                    --end;
                }
                while (end < block.contentEnd && p->content.data[end] != '\n')
                    ++end;
                block.contentEnd = end < block.contentEnd ? end + 1 : end;
            } else {
                // The first line holds the info string.
                uint32_t end = block.contentStart;
                while (end < block.contentEnd && p->content.data[end] != '\n')
                    ++end;
                PrStringView info = stringTrimBy(
                    PrStringView{p->content.data + block.contentStart, size_t(end - block.contentStart)},
                    prAsciiIsTabOrSpace);
                uint32_t mark = p->text.count;
                markdownAppendDecoded(info, &p->text);
                block.info = MarkdownRange{mark, p->text.count - mark};
                block.contentStart = end < block.contentEnd ? end + 1 : end;
            }
            break;
        case PrMarkdownNodeKind_List: {
            block.tight = true;
            for (MarkdownBlockIndex item = block.first; item != kMarkdownBlockNone && block.tight; item =
                    blockOf(p, item).next) {
                if (blockOf(p, item).lastLineBlank && blockOf(p, item).next != kMarkdownBlockNone) {
                    block.tight = false;
                    break;
                }
                for (MarkdownBlockIndex child = blockOf(p, item).first; child != kMarkdownBlockNone; child =
                        blockOf(p, child).next) {
                    bool followed = blockOf(p, item).next != kMarkdownBlockNone ||
                        blockOf(p, child).next != kMarkdownBlockNone;
                    if (followed && endsWithBlankLine(p, child)) {
                        block.tight = false;
                        break;
                    }
                }
            }
            break;
        }
        default:
            break;
    }
    return parent;
}

// Creates a block of `kind` as the last child of `parent`, or of the nearest ancestor that can contain it, closing the
// blocks on the way.
MarkdownBlockIndex addChild(MarkdownBlockParser* p, MarkdownBlockIndex parent, PrMarkdownNodeKind kind) {
    while (!canContain(blockOf(p, parent).kind, kind))
        parent = finalize(p, parent);
    MarkdownBlockIndex child = createBlock(p, kind);
    MarkdownParseBlock& block = blockOf(p, child);
    MarkdownParseBlock& owner = blockOf(p, parent);
    block.parent = parent;
    block.previous = owner.last;
    if (owner.last != kMarkdownBlockNone)
        blockOf(p, owner.last).next = child;
    else
        owner.first = child;
    owner.last = child;
    if (parent == 0)
        ++p->rootChildren;
    return child;
}

// ---------------------------------------------------------------------------------------------------------------------
// Lines

// Finds the first character from the offset that is not a space or a tab, and the indentation up to it.
void findFirstNonspace(MarkdownBlockParser* p) {
    uint32_t toTab = kTabStop - p->column % kTabStop;
    if (p->firstNonspace <= p->offset) {
        p->firstNonspace = p->offset;
        p->firstNonspaceColumn = p->column;
        while (true) {
            char c = peek(p, p->firstNonspace);
            if (c == ' ') {
                ++p->firstNonspace;
                ++p->firstNonspaceColumn;
                if (--toTab == 0)
                    toTab = kTabStop;
            } else if (c == '\t') {
                ++p->firstNonspace;
                p->firstNonspaceColumn += toTab;
                toTab = kTabStop;
            } else {
                break;
            }
        }
    }
    p->indent = p->firstNonspaceColumn - p->column;
    p->blank = isLineEnd(peek(p, p->firstNonspace));
}

// Advances the offset by `count` columns if `columns` is set, or by `count` bytes otherwise. Advancing by columns into
// a tab consumes the tab partially.
void advance(MarkdownBlockParser* p, uint32_t count, bool columns) {
    while (count > 0) {
        char c = peek(p, p->offset);
        if (c == '\0')
            break;
        if (c == '\t') {
            uint32_t toTab = kTabStop - p->column % kTabStop;
            if (columns) {
                p->partiallyConsumedTab = toTab > count;
                uint32_t advanced = count < toTab ? count : toTab;
                p->column += advanced;
                p->offset += p->partiallyConsumedTab ? 0 : 1;
                count -= advanced;
            } else {
                p->partiallyConsumedTab = false;
                p->column += toTab;
                ++p->offset;
                --count;
            }
        } else {
            p->partiallyConsumedTab = false;
            ++p->offset;
            ++p->column;
            --count;
        }
    }
}

void addLine(MarkdownBlockParser* p, MarkdownBlockIndex index) {
    MarkdownParseBlock& block = blockOf(p, index);
    PR_ASSERT(block.open && block.contentEnd == p->content.count);
    if (p->partiallyConsumedTab) {
        ++p->offset;
        uint32_t toTab = kTabStop - p->column % kTabStop;
        for (uint32_t i = 0; i < toTab; ++i)
            p->content.push(' ');
    }
    if (p->offset < p->line.count)
        p->content.append(p->line.data + p->offset, p->line.count - p->offset);
    block.contentEnd = p->content.count;
    block.lastLineSource = p->lineSource;
}

// Returns the length of the ATX heading's opening sequence and the spaces after it at `start`, or 0 if none starts
// there (commonmark/#atx-headings).
uint32_t atxHeadingStart(const MarkdownBlockParser* p, uint32_t start, uint8_t* outLevel) {
    uint32_t end = start;
    while (peek(p, end) == '#')
        ++end;
    uint32_t level = end - start;
    if (level == 0 || level > 6)
        return 0;
    if (isLineEnd(peek(p, end))) {
        *outLevel = uint8_t(level);
        return end - start;
    }
    if (!prAsciiIsTabOrSpace(peek(p, end)))
        return 0;
    while (prAsciiIsTabOrSpace(peek(p, end)))
        ++end;
    *outLevel = uint8_t(level);
    return end - start;
}

// Returns the length of the opening code fence at `start`, or 0 if none starts there (commonmark/#code-fence).
uint32_t openCodeFence(const MarkdownBlockParser* p, uint32_t start) {
    char fence = peek(p, start);
    if (fence != '`' && fence != '~')
        return 0;
    uint32_t end = start;
    while (peek(p, end) == fence)
        ++end;
    if (end - start < 3)
        return 0;
    // The info string of a backtick fence holds no backtick.
    if (fence == '`') {
        for (uint32_t i = end; !isLineEnd(peek(p, i)); ++i) {
            if (peek(p, i) == '`')
                return 0;
        }
    }
    return end - start;
}

// Returns the length of the closing code fence at `start`, or 0 if none is there.
uint32_t closeCodeFence(const MarkdownBlockParser* p, uint32_t start, char fence) {
    uint32_t end = start;
    while (peek(p, end) == fence)
        ++end;
    uint32_t length = end - start;
    if (length < 3)
        return 0;
    while (prAsciiIsTabOrSpace(peek(p, end)))
        ++end;
    return isLineEnd(peek(p, end)) ? length : 0;
}

// Returns 1 or 2 for a setext heading underline of '=' or '-' at `start`, or 0 if the line is not one
// (commonmark/#setext-heading-underline).
uint8_t setextUnderline(const MarkdownBlockParser* p, uint32_t start) {
    char c = peek(p, start);
    if (c != '=' && c != '-')
        return 0;
    uint32_t end = start;
    while (peek(p, end) == c)
        ++end;
    while (prAsciiIsTabOrSpace(peek(p, end)))
        ++end;
    if (!isLineEnd(peek(p, end)))
        return 0;
    return c == '=' ? 1 : 2;
}

// Returns true if a thematic break starts at `start` (commonmark/#thematic-breaks). If none does, records where the
// scan failed, since no thematic break starts before that offset either.
bool thematicBreak(MarkdownBlockParser* p, uint32_t start) {
    char c = peek(p, start);
    if (c != '*' && c != '_' && c != '-') {
        p->thematicBreakKillPosition = start;
        return false;
    }
    uint32_t count = 1;
    uint32_t index = start + 1;
    while (true) {
        char next = peek(p, index);
        if (next == c)
            ++count;
        else if (!prAsciiIsTabOrSpace(next))
            break;
        ++index;
    }
    if (count >= 3 && isLineEnd(peek(p, index)))
        return true;
    p->thematicBreakKillPosition = index;
    return false;
}

// The values of a list marker.
struct ListMarker {
    PrMarkdownListKind list;
    char bulletCharacter;
    char delimiter;
    uint32_t start;
};

// Returns the length of a footnote definition's label and the whitespace after it at `start`, from its "[^" through its
// "]:", or 0 if none starts there. Writes the label to `*outLabel`. A label holds no whitespace and no ']'.
uint32_t footnoteDefinitionStart(const MarkdownBlockParser* p, uint32_t start, MarkdownRange* outLabel) {
    if (peek(p, start) != '[' || peek(p, start + 1) != '^')
        return 0;
    uint32_t end = start + 2;
    while (true) {
        char c = peek(p, end);
        if (c == ']' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0')
            break;
        ++end;
    }
    if (end == start + 2 || peek(p, end) != ']' || peek(p, end + 1) != ':')
        return 0;
    *outLabel = MarkdownRange{start + 2, end - (start + 2)};
    end += 2;
    while (prAsciiIsTabOrSpace(peek(p, end)))
        ++end;
    return end - start;
}

// Returns the length of the list marker at `start`, or 0 if none starts there (commonmark/#list-marker). A marker that
// interrupts a paragraph must be followed by content, and an ordered one must be numbered 1.
uint32_t listMarker(const MarkdownBlockParser* p, uint32_t start, bool interruptsParagraph, ListMarker* out) {
    char c = peek(p, start);
    uint32_t end = start;
    if (c == '*' || c == '-' || c == '+') {
        ++end;
        *out = ListMarker{PrMarkdownListKind_Bullet, c, '\0', 0};
    } else if (prAsciiIsDigit(c)) {
        uint32_t number = 0;
        uint32_t digits = 0;
        while (digits < kMaxListDigits && prAsciiIsDigit(peek(p, end))) {
            number = number * 10 + uint32_t(peek(p, end) - '0');
            ++end;
            ++digits;
        }
        if (interruptsParagraph && number != 1)
            return 0;
        char delimiter = peek(p, end);
        if (delimiter != '.' && delimiter != ')')
            return 0;
        ++end;
        *out = ListMarker{PrMarkdownListKind_Ordered, '\0', delimiter, number};
    } else {
        return 0;
    }
    char next = peek(p, end);
    if (!prAsciiIsTabOrSpace(next) && !isLineEnd(next))
        return 0;
    if (interruptsParagraph) {
        uint32_t index = end;
        while (prAsciiIsTabOrSpace(peek(p, index)))
            ++index;
        if (isLineEnd(peek(p, index)))
            return 0;
    }
    return end - start;
}

// ---------------------------------------------------------------------------------------------------------------------
// Tables (gfm/#tables-extension)

// Returns true if `c` is whitespace within a table row: a space, a tab, a vertical tab or a form feed.
bool isTableSpace(char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

// Returns the length of the pipe and the whitespace after it at `start` in `text`, or 0 if no pipe is there.
size_t cellEnd(PrStringView text, size_t start) {
    if (start >= text.size || text.data[start] != '|')
        return 0;
    size_t end = start + 1;
    while (end < text.size && isTableSpace(text.data[end]))
        ++end;
    return end - start;
}

// Returns the length of the cell's characters at `start` in `text`, up to a pipe that is not escaped or a line ending.
size_t cellText(PrStringView text, size_t start) {
    size_t end = start;
    while (end < text.size) {
        char c = text.data[end];
        if (c == '\\' && end + 1 < text.size && markdownIsAsciiPunctuation(text.data[end + 1])) {
            end += 2;
            continue;
        }
        if (c == '|' || c == '\n' || c == '\r')
            break;
        ++end;
    }
    return end - start;
}

// Returns the length of the whitespace and the line ending at `start` in `text`, or 0 if no line ending follows it.
size_t rowEnd(PrStringView text, size_t start) {
    size_t end = start;
    while (end < text.size && isTableSpace(text.data[end]))
        ++end;
    if (end < text.size && text.data[end] == '\r')
        ++end;
    if (end < text.size && text.data[end] == '\n')
        return end + 1 - start;
    return 0;
}

// Returns true if a delimiter row starts at `start` in `text` (gfm/#delimiter-row): cells of dashes, each with an
// optional colon at either end, separated by pipes, with an optional pipe at either end of the row.
bool isDelimiterRow(PrStringView text, size_t start) {
    size_t index = start;
    if (index < text.size && text.data[index] == '|')
        ++index;
    bool cell = false;
    while (true) {
        size_t marker = index;
        while (index < text.size && isTableSpace(text.data[index]))
            ++index;
        if (index < text.size && text.data[index] == ':')
            ++index;
        size_t dashes = index;
        while (index < text.size && text.data[index] == '-')
            ++index;
        if (index == dashes) {
            // A row needs a cell, and only a pipe ends a cell that is not the last.
            if (!cell)
                return false;
            index = marker;
            break;
        }
        if (index < text.size && text.data[index] == ':')
            ++index;
        while (index < text.size && isTableSpace(text.data[index]))
            ++index;
        cell = true;
        if (index < text.size && text.data[index] == '|') {
            ++index;
            continue;
        }
        break;
    }
    return rowEnd(text, index) != 0;
}

// A cell of a row, as a range of the parser's content.
struct RowCell {
    uint32_t start;
    uint32_t size;
};

// Appends `text` to the parser's content with its escaped pipes unescaped and the whitespace at its ends trimmed, and
// returns its range.
RowCell addCellText(MarkdownBlockParser* p, PrStringView text) {
    uint32_t mark = p->content.count;
    for (size_t i = 0; i < text.size; ++i) {
        if (text.data[i] == '\\' && i + 1 < text.size && text.data[i + 1] == '|')
            continue;
        p->content.push(text.data[i]);
    }
    PrStringView written = prStringTrim(PrStringView{p->content.data + mark, size_t(p->content.count - mark)});
    return RowCell{uint32_t(written.data - p->content.data), uint32_t(written.size)};
}

// Splits `text` into the cells of its last row, whose text it adds to the parser's content. Returns false if the text
// is not rows. Writes to `*outParagraph` the offset in the text of the last row, which is 0 unless rows come before it.
// The text must not be in the parser's content, which grows.
bool splitRow(MarkdownBlockParser* p, PrStringView text, RawArray<RowCell>* cells, size_t* outParagraph) {
    cells->clear();
    *outParagraph = 0;
    size_t offset = cellEnd(text, 0);
    bool more = true;
    while (offset < text.size && more) {
        size_t cell = cellText(text, offset);
        size_t pipe = cellEnd(text, offset + cell);
        if (cell != 0 || pipe != 0)
            cells->push(addCellText(p, prStringSlice(text, offset, cell)));
        offset += cell + pipe;
        if (pipe != 0)
            continue;
        size_t end = rowEnd(text, offset);
        offset += end;
        if (end != 0 && offset != text.size) {
            // The rows before the last are a paragraph before the table.
            *outParagraph = offset;
            cells->clear();
            offset += cellEnd(text, offset);
        } else {
            more = false;
        }
    }
    return offset == text.size && cells->count != 0;
}

// Returns a copy of the line from its first non-whitespace character, which outlives changes to the parser's content.
void copyRestOfLine(const MarkdownBlockParser* p, RawArray<char>* out) {
    out->clear();
    out->append(p->line.data + p->firstNonspace, p->line.count - p->firstNonspace);
}

// Adds a row of `cells` to `table`, filled with empty cells to the table's columns, or cut to them.
void addRow(MarkdownBlockParser* p, MarkdownBlockIndex table, const RawArray<RowCell>& cells, bool header) {
    MarkdownBlockIndex row = addChild(p, table, PrMarkdownNodeKind_TableRow);
    blockOf(p, row).open = false;
    blockOf(p, row).header = header;
    uint32_t columns = blockOf(p, table).columns;
    for (uint32_t i = 0; i < columns; ++i) {
        MarkdownBlockIndex cell = addChild(p, row, PrMarkdownNodeKind_TableCell);
        MarkdownParseBlock& added = blockOf(p, cell);
        added.open = false;
        added.header = header;
        added.alignment = p->alignments.data[blockOf(p, table).alignmentStart + i];
        if (i < cells.count) {
            added.contentStart = cells.data[i].start;
            added.contentEnd = cells.data[i].start + cells.data[i].size;
        } else {
            added.contentStart = p->content.count;
            added.contentEnd = p->content.count;
        }
    }
    MarkdownParseBlock& owner = blockOf(p, table);
    ++owner.rows;
    if (cells.count < columns)
        owner.filledCells += columns - cells.count;
}

// Makes the paragraph `paragraph` a table if the line is a delimiter row with as many cells as the paragraph's last
// line. If its last line is not a header row of that many cells, the paragraph never becomes a table.
void tryTableHeader(MarkdownBlockParser* p, MarkdownBlockIndex paragraph) {
    if (blockOf(p, paragraph).tableVisited)
        return;
    RawArray<char> line;
    copyRestOfLine(p, &line);
    PrStringView delimiterText{line.data, line.count};
    if (!isDelimiterRow(delimiterText, 0)) {
        line.reset();
        return;
    }
    RawArray<RowCell> delimiters;
    RawArray<RowCell> headers;
    RawArray<char> content;
    size_t ignored = 0;
    size_t paragraphEnd = 0;
    uint32_t mark = p->content.count;
    bool made = splitRow(p, delimiterText, &delimiters, &ignored);
    if (made) {
        const MarkdownParseBlock& from = blockOf(p, paragraph);
        content.append(p->content.data + from.contentStart, from.contentEnd - from.contentStart);
        made = splitRow(p, PrStringView{content.data, content.count}, &headers, &paragraphEnd) &&
            headers.count == delimiters.count;
        if (!made)
            blockOf(p, paragraph).tableVisited = true;
    }
    if (made) {
        MarkdownParseBlock& table = blockOf(p, paragraph);
        table.kind = PrMarkdownNodeKind_Table;
        table.columns = delimiters.count;
        table.alignmentStart = p->alignments.count;
        for (uint32_t i = 0; i < delimiters.count; ++i) {
            PrStringView cell{p->content.data + delimiters.data[i].start, delimiters.data[i].size};
            bool left = cell.size != 0 && cell.data[0] == ':';
            bool right = cell.size != 0 && cell.data[cell.size - 1] == ':';
            PrMarkdownAlignment alignment = PrMarkdownAlignment_None;
            if (left && right)
                alignment = PrMarkdownAlignment_Center;
            else if (left)
                alignment = PrMarkdownAlignment_Left;
            else if (right)
                alignment = PrMarkdownAlignment_Right;
            p->alignments.push(alignment);
        }
        if (paragraphEnd != 0) {
            // The lines before the header row stay a paragraph, which comes before the table. Its escaped pipes are
            // unescaped, as the cells' are.
            MarkdownBlockIndex before = createBlock(p, PrMarkdownNodeKind_Paragraph);
            RowCell text = addCellText(p, PrStringView{content.data, paragraphEnd});
            MarkdownParseBlock& added = blockOf(p, before);
            MarkdownParseBlock& converted = blockOf(p, paragraph);
            added.open = false;
            added.sourceStart = converted.sourceStart;
            added.contentStart = text.start;
            added.contentEnd = text.start + text.size;
            added.parent = converted.parent;
            if (converted.parent == 0)
                ++p->rootChildren;
            added.next = paragraph;
            added.previous = converted.previous;
            if (converted.previous != kMarkdownBlockNone)
                blockOf(p, converted.previous).next = before;
            else
                blockOf(p, converted.parent).first = before;
            converted.previous = before;
            converted.sourceStart = converted.lastLineSource;
        }
        addRow(p, paragraph, headers, true);
        advance(p, p->line.count - 1 - p->offset, false);
    } else {
        p->content.resize(mark);
    }
    line.reset();
    delimiters.reset();
    headers.reset();
    content.reset();
}

// Adds the line to `table` as a row. Returns false if the line is not a row.
bool tryTableRow(MarkdownBlockParser* p, MarkdownBlockIndex table) {
    if (p->blank || blockOf(p, table).filledCells > kMaxFilledCells)
        return false;
    RawArray<char> line;
    copyRestOfLine(p, &line);
    RawArray<RowCell> cells;
    size_t ignored = 0;
    uint32_t mark = p->content.count;
    bool row = splitRow(p, PrStringView{line.data, line.count}, &cells, &ignored);
    if (row) {
        addRow(p, table, cells, false);
        advance(p, p->line.count - 1 - p->offset, false);
    } else {
        p->content.resize(mark);
    }
    line.reset();
    cells.reset();
    return row;
}

// Returns true if the line, from its first non-whitespace character, is a row of a table.
bool continuesTable(MarkdownBlockParser* p) {
    RawArray<char> line;
    copyRestOfLine(p, &line);
    RawArray<RowCell> cells;
    size_t ignored = 0;
    uint32_t mark = p->content.count;
    bool row = splitRow(p, PrStringView{line.data, line.count}, &cells, &ignored);
    p->content.resize(mark);
    line.reset();
    cells.reset();
    return row;
}

// Returns true if the line starts with a list item marker followed by a task list item marker
// (gfm/#task-list-items-extension). The line is read from its start, before any container's prefix, so an item in a
// block quote is not a task.
bool taskListMarker(const MarkdownBlockParser* p) {
    uint32_t index = 0;
    while (isTableSpace(peek(p, index)))
        ++index;
    char c = peek(p, index);
    if (c == '-' || c == '+' || c == '*') {
        ++index;
    } else if (prAsciiIsDigit(c)) {
        while (prAsciiIsDigit(peek(p, index)))
            ++index;
        // Any character but a line ending follows the number.
        if (isLineEnd(peek(p, index)))
            return false;
        ++index;
    } else {
        return false;
    }
    if (!isTableSpace(peek(p, index)))
        return false;
    while (isTableSpace(peek(p, index)))
        ++index;
    bool box = peek(p, index) == '[' && peek(p, index + 2) == ']' && (peek(p, index + 1) == ' ' || peek(p, index + 1) == 'x');
    return box && isTableSpace(peek(p, index + 3));
}

// Matches the line's prefix against each open block, and returns the deepest block whose prefix it matches. Sets
// `*allMatched` if the line continues every open block. Returns kMarkdownBlockNone if the line closed a code fence,
// which ends the line.
MarkdownBlockIndex checkOpenBlocks(MarkdownBlockParser* p, bool* allMatched) {
    *allMatched = false;
    MarkdownBlockIndex container = 0;
    while (blockOf(p, container).last != kMarkdownBlockNone && blockOf(p, blockOf(p, container).last).open) {
        container = blockOf(p, container).last;
        findFirstNonspace(p);
        MarkdownParseBlock& block = blockOf(p, container);
        bool matched = true;
        switch (block.kind) {
            case PrMarkdownNodeKind_BlockQuote:
                matched = p->indent <= 3 && peek(p, p->firstNonspace) == '>';
                if (matched) {
                    advance(p, p->indent + 1, true);
                    if (prAsciiIsTabOrSpace(peek(p, p->offset)))
                        advance(p, 1, true);
                }
                break;
            case PrMarkdownNodeKind_Item:
                if (p->indent >= block.markerOffset + block.markerWidth)
                    advance(p, block.markerOffset + block.markerWidth, true);
                else if (p->blank && block.first != kMarkdownBlockNone)
                    advance(p, p->firstNonspace - p->offset, false);
                else
                    matched = false;
                break;
            case PrMarkdownNodeKind_CodeBlock:
                if (!block.fenced) {
                    if (p->indent >= kCodeIndent)
                        advance(p, kCodeIndent, true);
                    else if (p->blank)
                        advance(p, p->firstNonspace - p->offset, false);
                    else
                        matched = false;
                } else {
                    uint32_t closing = 0;
                    if (p->indent <= 3 && peek(p, p->firstNonspace) == block.fenceCharacter)
                        closing = closeCodeFence(p, p->firstNonspace, block.fenceCharacter);
                    if (closing >= block.fenceLength) {
                        p->current = finalize(p, container);
                        return kMarkdownBlockNone;
                    }
                    for (uint32_t i = block.fenceOffset; i > 0 && prAsciiIsTabOrSpace(peek(p, p->offset)); --i)
                        advance(p, 1, true);
                }
                break;
            case PrMarkdownNodeKind_Heading:
                matched = false;
                break;
            case PrMarkdownNodeKind_HtmlBlock:
                matched = block.htmlKind <= 5 || !p->blank;
                break;
            case PrMarkdownNodeKind_Paragraph:
                matched = !p->blank;
                break;
            case PrMarkdownNodeKind_Table:
                matched = continuesTable(p);
                break;
            case PrMarkdownNodeKind_FootnoteDefinition:
                // An empty line, or one indented by four columns, continues a footnote definition, which a line that is
                // only spaces does not.
                if (p->indent >= 4)
                    advance(p, 4, true);
                else
                    matched = p->line.data[0] == '\n';
                break;
            default:
                break;
        }
        if (!matched)
            return blockOf(p, container).parent;
    }
    *allMatched = true;
    return container;
}

// Opens the blocks that the rest of the line starts, under `*container`, and leaves `*container` at the deepest one.
void openNewBlocks(MarkdownBlockParser* p, const PrMarkdownOptions& options, MarkdownBlockIndex* container,
        bool allMatched) {
    bool maybeLazy = blockOf(p, p->current).kind == PrMarkdownNodeKind_Paragraph;
    PrMarkdownNodeKind kind = blockOf(p, *container).kind;
    uint32_t opened = 0;
    while (kind != PrMarkdownNodeKind_CodeBlock && kind != PrMarkdownNodeKind_HtmlBlock) {
        ++opened;
        findFirstNonspace(p);
        bool indented = p->indent >= kCodeIndent;
        char c = peek(p, p->firstNonspace);
        uint8_t level = 0;
        uint32_t matched = 0;
        uint8_t htmlKind = 0;
        ListMarker marker{};
        MarkdownRange label{};
        if (!indented && c == '>') {
            advance(p, p->firstNonspace + 1 - p->offset, false);
            if (prAsciiIsTabOrSpace(peek(p, p->offset)))
                advance(p, 1, true);
            *container = addChild(p, *container, PrMarkdownNodeKind_BlockQuote);
        } else if (!indented && (matched = atxHeadingStart(p, p->firstNonspace, &level)) != 0) {
            advance(p, p->firstNonspace + matched - p->offset, false);
            *container = addChild(p, *container, PrMarkdownNodeKind_Heading);
            blockOf(p, *container).level = level;
        } else if (!indented && (matched = openCodeFence(p, p->firstNonspace)) != 0) {
            *container = addChild(p, *container, PrMarkdownNodeKind_CodeBlock);
            MarkdownParseBlock& block = blockOf(p, *container);
            block.fenced = true;
            block.fenceCharacter = c;
            block.fenceLength = uint8_t(matched > 255 ? 255 : matched);
            block.fenceOffset = uint8_t(p->firstNonspace - p->offset);
            advance(p, p->firstNonspace + matched - p->offset, false);
        } else if (!indented && (htmlKind = markdownHtmlBlockStart(lineView(p), p->firstNonspace,
                kind == PrMarkdownNodeKind_Paragraph)) != 0) {
            *container = addChild(p, *container, PrMarkdownNodeKind_HtmlBlock);
            blockOf(p, *container).htmlKind = htmlKind;
            // The tag is part of the block's text, so the offset stays.
        } else if (!indented && kind == PrMarkdownNodeKind_Paragraph && (level = setextUnderline(p, p->firstNonspace)) != 0) {
            if (resolveDefinitions(p, *container)) {
                MarkdownParseBlock& block = blockOf(p, *container);
                block.kind = PrMarkdownNodeKind_Heading;
                block.level = level;
                block.setext = true;
                advance(p, p->line.count - 1 - p->offset, false);
            }
        } else if (!indented && !(kind == PrMarkdownNodeKind_Paragraph && !allMatched) &&
                p->thematicBreakKillPosition <= p->firstNonspace && thematicBreak(p, p->firstNonspace)) {
            *container = addChild(p, *container, PrMarkdownNodeKind_ThematicBreak);
            advance(p, p->line.count - 1 - p->offset, false);
        } else if (!indented && (options.extensions & PrMarkdownExtensionFlag_Footnotes) != 0 &&
                opened < kMaxOpenedPerLine && (matched = footnoteDefinitionStart(p, p->firstNonspace, &label))) {
            uint32_t mark = p->text.count;
            p->text.append(p->line.data + label.start, label.size);
            advance(p, p->firstNonspace + matched - p->offset, false);
            *container = addChild(p, *container, PrMarkdownNodeKind_FootnoteDefinition);
            blockOf(p, *container).info = MarkdownRange{mark, label.size};
        } else if ((!indented || kind == PrMarkdownNodeKind_List) && p->indent < 4 && opened < kMaxOpenedPerLine &&
                (matched = listMarker(p, p->firstNonspace, kind == PrMarkdownNodeKind_Paragraph, &marker)) != 0) {
            advance(p, p->firstNonspace + matched - p->offset, false);
            bool savedTab = p->partiallyConsumedTab;
            uint32_t savedOffset = p->offset;
            uint32_t savedColumn = p->column;
            while (p->column - savedColumn <= 5 && prAsciiIsTabOrSpace(peek(p, p->offset)))
                advance(p, 1, true);
            uint32_t spaces = p->column - savedColumn;
            uint32_t markerWidth = 0;
            // Content that starts five or more columns after the marker is indented code, so the item's content starts
            // one column after the marker (commonmark/#list-items).
            if (spaces >= 5 || spaces < 1 || isLineEnd(peek(p, p->offset))) {
                markerWidth = matched + 1;
                p->offset = savedOffset;
                p->column = savedColumn;
                p->partiallyConsumedTab = savedTab;
                if (spaces > 0)
                    advance(p, 1, true);
            } else {
                markerWidth = matched + spaces;
            }
            const MarkdownParseBlock& list = blockOf(p, *container);
            bool continues = kind == PrMarkdownNodeKind_List && list.list == marker.list &&
                list.delimiter == marker.delimiter && list.bulletCharacter == marker.bulletCharacter;
            if (!continues) {
                *container = addChild(p, *container, PrMarkdownNodeKind_List);
                MarkdownParseBlock& added = blockOf(p, *container);
                added.list = marker.list;
                added.bulletCharacter = marker.bulletCharacter;
                added.delimiter = marker.delimiter;
                added.start = marker.start;
            }
            *container = addChild(p, *container, PrMarkdownNodeKind_Item);
            MarkdownParseBlock& item = blockOf(p, *container);
            item.list = marker.list;
            item.bulletCharacter = marker.bulletCharacter;
            item.delimiter = marker.delimiter;
            item.start = marker.start;
            item.markerOffset = p->indent;
            item.markerWidth = markerWidth;
        } else if (indented && !maybeLazy && !p->blank) {
            advance(p, kCodeIndent, true);
            *container = addChild(p, *container, PrMarkdownNodeKind_CodeBlock);
        } else if (!indented && kind == PrMarkdownNodeKind_Paragraph &&
                (options.extensions & PrMarkdownExtensionFlag_Tables) != 0) {
            tryTableHeader(p, *container);
            break;
        } else if (!indented && kind == PrMarkdownNodeKind_Table &&
                (options.extensions & PrMarkdownExtensionFlag_Tables) != 0) {
            if (!tryTableRow(p, *container))
                break;
        } else if (kind == PrMarkdownNodeKind_Item && (options.extensions & PrMarkdownExtensionFlag_TaskLists) != 0 &&
                taskListMarker(p)) {
            // The task list item marker is the first three characters of the item's content. The item is checked if the
            // line holds "[x]" or "[X]" anywhere, which is what GFM's HTML gives.
            bool checked = false;
            for (uint32_t i = 0; i + 2 < p->line.count; ++i) {
                if (p->line.data[i] == '[' && (p->line.data[i + 1] == 'x' || p->line.data[i + 1] == 'X') &&
                        p->line.data[i + 2] == ']')
                    checked = true;
            }
            blockOf(p, *container).task = checked ? PrMarkdownTask_Checked : PrMarkdownTask_Unchecked;
            advance(p, 3, false);
            break;
        } else {
            break;
        }
        kind = blockOf(p, *container).kind;
        if (acceptsLines(kind))
            break;
        maybeLazy = false;
    }
}

// Removes the closing sequence of an ATX heading from the end of the line (commonmark/#atx-headings).
void chopClosingSequence(MarkdownBlockParser* p) {
    // The line ends with '\n', which stays.
    uint32_t end = p->line.count - 1;
    while (end > p->offset && prAsciiIsTabOrSpace(p->line.data[end - 1]))
        --end;
    uint32_t hashes = end;
    while (hashes > p->offset && p->line.data[hashes - 1] == '#')
        --hashes;
    if (hashes != end && (hashes == p->offset || prAsciiIsTabOrSpace(p->line.data[hashes - 1]))) {
        end = hashes;
        while (end > p->offset && prAsciiIsTabOrSpace(p->line.data[end - 1]))
            --end;
    }
    p->line.data[end] = '\n';
    p->line.resize(end + 1);
}

void addTextToContainer(MarkdownBlockParser* p, MarkdownBlockIndex container, MarkdownBlockIndex lastMatched) {
    findFirstNonspace(p);
    if (p->blank && blockOf(p, container).last != kMarkdownBlockNone)
        blockOf(p, blockOf(p, container).last).lastLineBlank = true;

    // A block quote's lines are never blank, since they start with '>'. Blank lines in fenced code and the blank first
    // line of an empty item do not decide whether a list is tight.
    const MarkdownParseBlock& block = blockOf(p, container);
    bool lastLineBlank = p->blank && block.kind != PrMarkdownNodeKind_BlockQuote &&
        block.kind != PrMarkdownNodeKind_Heading && block.kind != PrMarkdownNodeKind_ThematicBreak &&
        !(block.kind == PrMarkdownNodeKind_CodeBlock && block.fenced) &&
        !(block.kind == PrMarkdownNodeKind_Item && block.first == kMarkdownBlockNone &&
            block.startLine == p->lineNumber);
    blockOf(p, container).lastLineBlank = lastLineBlank;
    for (MarkdownBlockIndex ancestor = blockOf(p, container).parent; ancestor != kMarkdownBlockNone;
            ancestor = blockOf(p, ancestor).parent)
        blockOf(p, ancestor).lastLineBlank = false;

    // A line that matches fewer prefixes than the open blocks have and starts no block continues an open paragraph
    // lazily (commonmark/#lazy-continuation-line).
    if (p->current != lastMatched && container == lastMatched && !p->blank &&
            blockOf(p, p->current).kind == PrMarkdownNodeKind_Paragraph) {
        addLine(p, p->current);
        return;
    }
    while (p->current != lastMatched)
        p->current = finalize(p, p->current);

    PrMarkdownNodeKind kind = blockOf(p, container).kind;
    if (kind == PrMarkdownNodeKind_CodeBlock) {
        addLine(p, container);
    } else if (kind == PrMarkdownNodeKind_HtmlBlock) {
        addLine(p, container);
        uint8_t htmlKind = blockOf(p, container).htmlKind;
        PrStringView rest = prStringSlice(lineView(p), p->firstNonspace, p->line.count - p->firstNonspace);
        if (htmlKind <= 5 && markdownHtmlBlockEnds(htmlKind, rest))
            container = finalize(p, container);
    } else if (p->blank) {
        // A blank line adds nothing.
    } else if (acceptsLines(kind)) {
        if (kind == PrMarkdownNodeKind_Heading && !blockOf(p, container).setext)
            chopClosingSequence(p);
        advance(p, p->firstNonspace - p->offset, false);
        addLine(p, container);
    } else {
        container = addChild(p, container, PrMarkdownNodeKind_Paragraph);
        advance(p, p->firstNonspace - p->offset, false);
        addLine(p, container);
    }
    p->current = container;
}

// Copies the line of `source` at `start`, which ends at `end`, into the parser's line, with its line ending made '\n'
// and U+0000 replaced (commonmark/#insecure-characters).
void readLine(MarkdownBlockParser* p, PrStringView source, uint32_t start, uint32_t end) {
    p->line.clear();
    uint32_t run = start;
    for (uint32_t i = start; i < end; ++i) {
        char c = source.data[i];
        if (c == '\0') {
            p->line.append(source.data + run, i - run);
            p->line.append("\xef\xbf\xbd", 3);
            run = i + 1;
        } else if (c == '\n' || c == '\r') {
            p->line.append(source.data + run, i - run);
            run = end;
            break;
        }
    }
    if (run < end)
        p->line.append(source.data + run, end - run);
    p->line.push('\n');
}

void processLine(MarkdownBlockParser* p, const PrMarkdownOptions& options) {
    p->offset = 0;
    p->column = 0;
    p->firstNonspace = 0;
    p->firstNonspaceColumn = 0;
    p->indent = 0;
    p->thematicBreakKillPosition = 0;
    p->blank = false;
    p->partiallyConsumedTab = false;
    if (p->lineSource == 0 && p->line.count >= 3 && prStringStartsWith(lineView(p), "\xef\xbb\xbf"))
        p->offset = 3;
    ++p->lineNumber;

    bool allMatched = true;
    MarkdownBlockIndex lastMatched = checkOpenBlocks(p, &allMatched);
    if (lastMatched == kMarkdownBlockNone)
        return;
    MarkdownBlockIndex container = lastMatched;
    openNewBlocks(p, options, &container, allMatched);
    addTextToContainer(p, container, lastMatched);
}

// ---------------------------------------------------------------------------------------------------------------------
// Building

void appendHtml(const PrMarkdownOptions& options, MarkdownTree* tree, MarkdownNodeIndex parent, PrStringView html) {
    MarkdownNodeIndex text = markdownTreeAppend(tree, parent, PrMarkdownNodeKind_Text);
    uint32_t mark = tree->text.count;
    if (options.rawHtml == PrMarkdownRawHtml_Filter)
        markdownAppendFilteredHtml(html, &tree->text);
    else
        tree->text.append(html.data, uint32_t(html.size));
    tree->nodes.data[text].values.text = MarkdownRange{mark, tree->text.count - mark};
    tree->nodes.data[text].values.textKind =
        options.rawHtml == PrMarkdownRawHtml_Escape ? PrMarkdownTextKind_Plain : PrMarkdownTextKind_Html;
}

// Returns the kind of alert whose marker is the first line of `content`, alone on its line but for spaces and tabs, or
// PrMarkdownAlert_None. Writes the length of the line, with its line ending, to `*outLength`.
PrMarkdownAlert alertMarker(PrStringView content, size_t* outLength) {
    static const char* const kMarkers[] = {"[!NOTE]", "[!TIP]", "[!IMPORTANT]", "[!WARNING]", "[!CAUTION]"};
    size_t end = prStringFindByte(content, '\n');
    PrStringView line = stringTrimBy(prStringSlice(content, 0, end), prAsciiIsTabOrSpace);
    for (size_t i = 0; i < countOf(kMarkers); ++i) {
        if (prAsciiEqualsFold(line, kMarkers[i])) {
            *outLength = end < content.size ? end + 1 : end;
            return PrMarkdownAlert(PrMarkdownAlert_Note + i);
        }
    }
    return PrMarkdownAlert_None;
}

// Appends to `out` the text of the inlines under `node`, as an element's text content: the characters of its text and
// code, without raw HTML, and without the descriptions of images, which are attributes.
void appendTextContent(MarkdownTree* tree, MarkdownNodeIndex node, RawArray<char>* out) {
    RawArray<MarkdownNodeIndex> pending;
    for (MarkdownNodeIndex child = tree->nodes.data[node].last; child != kMarkdownNodeNone;
            child = tree->nodes.data[child].previous)
        pending.push(child);
    while (pending.count != 0) {
        MarkdownNodeIndex next = pending.last();
        pending.pop();
        const MarkdownValues& values = tree->nodes.data[next].values;
        if (values.kind == PrMarkdownNodeKind_Image)
            continue;
        if (values.kind == PrMarkdownNodeKind_Text && values.textKind == PrMarkdownTextKind_Plain)
            out->append(tree->text.data + values.text.start, values.text.size);
        for (MarkdownNodeIndex child = tree->nodes.data[next].last; child != kMarkdownNodeNone;
                child = tree->nodes.data[child].previous)
            pending.push(child);
    }
    pending.reset();
}

// Appends the slug of `text`, which is a heading's id, to `out`: the text lowercased, with each character that is not a
// letter, a mark, a number, a connector, a hyphen or a space removed, and each space made a hyphen.
void appendSlug(PrStringView text, RawArray<char>* out) {
    size_t index = 0;
    while (index < text.size) {
        uint32_t codePoint = 0;
        index += prUtf8Decode(text, index, &codePoint);
        uint32_t lower[3];
        uint32_t count = prUnicodeLowercase(codePoint, lower);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t c = lower[i];
            bool kept = c == '-' || c == ' ';
            switch (prUnicodeGeneralCategory(c)) {
                case PrUnicodeGeneralCategory_UppercaseLetter:
                case PrUnicodeGeneralCategory_LowercaseLetter:
                case PrUnicodeGeneralCategory_TitlecaseLetter:
                case PrUnicodeGeneralCategory_ModifierLetter:
                case PrUnicodeGeneralCategory_OtherLetter:
                case PrUnicodeGeneralCategory_NonspacingMark:
                case PrUnicodeGeneralCategory_SpacingMark:
                case PrUnicodeGeneralCategory_EnclosingMark:
                case PrUnicodeGeneralCategory_DecimalNumber:
                case PrUnicodeGeneralCategory_LetterNumber:
                case PrUnicodeGeneralCategory_OtherNumber:
                case PrUnicodeGeneralCategory_ConnectorPunctuation:
                    kept = true;
                    break;
                default:
                    break;
            }
            if (!kept)
                continue;
            char bytes[kPrUtf8MaxBytes];
            uint32_t size = c == ' ' ? 1 : prUtf8Encode(c, bytes);
            if (c == ' ')
                bytes[0] = '-';
            out->append(bytes, size);
        }
    }
}

// Returns true if the line `line`, which has no line ending yet, is parsed only once its line ending arrives, since the
// rest of the line can still turn it into something that shows otherwise: a link reference definition, which shows
// nothing, raw HTML, or a line that is only a marker so far, such as "--" before a setext underline or a thematic
// break, "1." before an item's content, or a fence. Text after a marker is that block's content whatever follows, so a
// line such as "- an item" or "# A heading" is parsed as it arrives.
bool waitsForLineEnd(PrStringView line) {
    size_t index = 0;
    while (true) {
        while (index < line.size && prAsciiIsTabOrSpace(line.data[index]))
            ++index;
        if (index < line.size && line.data[index] == '>')
            ++index;
        else
            break;
    }
    if (index == line.size)
        return true;
    char first = line.data[index];
    if (first == '[' || first == '<')
        return true;
    bool digits = prAsciiIsDigit(first);
    bool marker = first == '-' || first == '=' || first == '*' || first == '_' || first == '+' || first == '#' ||
        first == '`' || first == '~';
    if (!digits && !marker)
        return false;
    for (size_t i = index; i < line.size; ++i) {
        char c = line.data[i];
        bool continues = prAsciiIsTabOrSpace(c) || c == first || (digits && (prAsciiIsDigit(c) || c == '.' || c == ')'));
        if (!continues)
            return false;
    }
    return true;
}

} // namespace

void markdownBlockParserReset(MarkdownBlockParser* parser) {
    parser->blocks.reset();
    parser->content.reset();
    parser->line.reset();
    parser->text.reset();
    parser->definitions.reset();
    parser->alignments.reset();
}

void markdownParseBlocks(MarkdownBlockParser* parser, const PrMarkdownOptions& options, PrStringView source,
        uint32_t start, bool ended) {
    MarkdownBlockParser* p = parser;
    p->blocks.clear();
    p->content.clear();
    p->text.clear();
    p->definitions.clear();
    p->alignments.clear();
    p->lineNumber = 0;
    p->lineSource = start;
    p->current = createBlock(p, kDocumentKind);
    p->rootChildren = 0;
    p->finalEnd = start;
    p->finalCount = 0;

    uint32_t size = uint32_t(source.size);
    uint32_t lineStart = start;
    while (lineStart < size) {
        uint32_t end = lineStart;
        while (end < size && source.data[end] != '\n' && source.data[end] != '\r')
            ++end;
        bool complete = end < size;
        if (complete && source.data[end] == '\r') {
            // A CR at the end of an open source may be the first half of a CRLF.
            if (end + 1 < size && source.data[end + 1] == '\n')
                ++end;
            else if (end + 1 == size && !ended)
                complete = false;
        }
        uint32_t next = complete ? end + 1 : size;
        if (!complete && !ended && waitsForLineEnd(prStringSlice(source, lineStart, end - lineStart)))
            break;
        p->lineSource = lineStart;
        readLine(p, source, lineStart, next);
        processLine(p, options);
        if (complete || ended) {
            MarkdownBlockIndex last = p->blocks.data[0].last;
            uint32_t children = p->rootChildren;
            if (!ended && last != kMarkdownBlockNone && p->blocks.data[last].open) {
                p->finalCount = children - 1;
                p->finalEnd = p->blocks.data[last].sourceStart;
            } else {
                p->finalCount = children;
                p->finalEnd = next;
            }
        }
        lineStart = next;
    }
    while (p->current != 0)
        p->current = finalize(p, p->current);
    finalize(p, 0);
    if (ended) {
        p->finalCount = p->rootChildren;
        p->finalEnd = size;
    }
}

void markdownBuildBlock(MarkdownBlockParser* parser, const PrMarkdownOptions& options, MarkdownBlockIndex block,
        MarkdownTree* tree, MarkdownInlineParser* inlines, MarkdownResolveFn resolve, void* PR_NULLABLE context) {
    // The parse blocks to build, each with the tree node to build it under, and the bytes of its content that come
    // before what it holds, which an alert's marker is.
    struct Pending {
        MarkdownBlockIndex block;
        MarkdownNodeIndex parent;
        uint32_t skip;
    };
    RawArray<Pending> pending;
    RawArray<char> slug;
    pending.push(Pending{block, 0, 0});
    while (pending.count != 0) {
        Pending next = pending.last();
        pending.pop();
        const MarkdownParseBlock& from = parser->blocks.data[next.block];
        PrStringView content{
            parser->content.data + from.contentStart + next.skip,
            size_t(from.contentEnd - from.contentStart - next.skip),
        };
        PrMarkdownNodeKind kind = from.kind;
        if (kind == PrMarkdownNodeKind_HtmlBlock && options.rawHtml == PrMarkdownRawHtml_Escape)
            kind = PrMarkdownNodeKind_Paragraph;
        MarkdownNodeIndex node = markdownTreeAppend(tree, next.parent, kind);
        MarkdownValues& values = tree->nodes.data[node].values;
        switch (from.kind) {
            case PrMarkdownNodeKind_Paragraph:
                markdownParseInlines(inlines, options, tree, node, content, resolve, context);
                break;
            case PrMarkdownNodeKind_Heading:
                values.level = from.level;
                markdownParseInlines(inlines, options, tree, node, content, resolve, context);
                if ((options.extensions & PrMarkdownExtensionFlag_HeadingIds) != 0) {
                    // The heading's slug, which the document makes unique among its headings.
                    slug.clear();
                    appendTextContent(tree, node, &slug);
                    uint32_t mark = tree->text.count;
                    appendSlug(PrStringView{slug.data, slug.count}, &tree->text);
                    tree->nodes.data[node].values.info = MarkdownRange{mark, tree->text.count - mark};
                }
                break;
            case PrMarkdownNodeKind_List:
                values.list = from.list;
                values.start = from.start;
                values.tight = from.tight;
                break;
            case PrMarkdownNodeKind_CodeBlock: {
                PrStringView info{parser->text.data + from.info.start, from.info.size};
                values.info = markdownTreeAddText(tree, info);
                if (content.size != 0) {
                    MarkdownRange range = markdownTreeAddText(tree, content);
                    MarkdownNodeIndex text = markdownTreeAppend(tree, node, PrMarkdownNodeKind_Text);
                    tree->nodes.data[text].values.text = range;
                }
                break;
            }
            case PrMarkdownNodeKind_Item:
                values.task = from.task;
                break;
            case PrMarkdownNodeKind_FootnoteDefinition: {
                PrStringView label{parser->text.data + from.info.start, from.info.size};
                values.info = markdownTreeAddText(tree, label);
                break;
            }
            case PrMarkdownNodeKind_TableCell:
                values.alignment = from.alignment;
                values.header = from.header;
                markdownParseInlines(inlines, options, tree, node, content, resolve, context);
                break;
            case PrMarkdownNodeKind_HtmlBlock:
                if (options.rawHtml == PrMarkdownRawHtml_Escape) {
                    while (content.size != 0 && content.data[content.size - 1] == '\n')
                        --content.size;
                }
                appendHtml(options, tree, node, content);
                break;
            default:
                break;
        }
        // A block quote whose first paragraph starts with an alert's marker is an alert, whose paragraph is what
        // follows the marker. A paragraph of only the marker is left out.
        uint32_t firstSkip = 0;
        bool firstDropped = false;
        if (from.kind == PrMarkdownNodeKind_BlockQuote && (options.extensions & PrMarkdownExtensionFlag_Alerts) != 0 &&
                from.first != kMarkdownBlockNone &&
                parser->blocks.data[from.first].kind == PrMarkdownNodeKind_Paragraph) {
            const MarkdownParseBlock& paragraph = parser->blocks.data[from.first];
            PrStringView text{
                parser->content.data + paragraph.contentStart,
                size_t(paragraph.contentEnd - paragraph.contentStart),
            };
            size_t length = 0;
            PrMarkdownAlert alert = alertMarker(text, &length);
            if (alert != PrMarkdownAlert_None) {
                tree->nodes.data[node].values.kind = PrMarkdownNodeKind_Alert;
                tree->nodes.data[node].values.alert = alert;
                firstSkip = uint32_t(length);
                firstDropped = prStringTrim(prStringSlice(text, length, text.size - length)).size == 0;
            }
        }

        // Children are pushed last first, so that they are built in order. A table's first row is its head, and the
        // rows after it are its body.
        MarkdownNodeIndex head = node;
        MarkdownNodeIndex body = node;
        if (from.kind == PrMarkdownNodeKind_Table) {
            head = markdownTreeAppend(tree, node, PrMarkdownNodeKind_TableHead);
            if (from.first != kMarkdownBlockNone && parser->blocks.data[from.first].next != kMarkdownBlockNone)
                body = markdownTreeAppend(tree, node, PrMarkdownNodeKind_TableBody);
        }
        for (MarkdownBlockIndex child = from.last; child != kMarkdownBlockNone;
                child = parser->blocks.data[child].previous) {
            bool first = child == from.first;
            if (first && firstDropped)
                continue;
            pending.push(Pending{child, first ? head : body, first ? firstSkip : 0});
        }
    }
    pending.reset();
    slug.reset();
}

} // namespace pr_impl
