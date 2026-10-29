// Markdown inlines: the content of a paragraph or a heading, parsed into inline nodes.

#include "markdown/src/markdown_inline.h"

#include "pr/ascii.h"
#include "pr/mem.h"
#include "pr/string_ops.h"
#include "pr/unicode.h"
#include "pr/utf8.h"

#include "markdown/src/markdown_scan.h"

namespace pr_impl {

namespace {

// The position of no delimiter.
constexpr uint32_t kDelimiterNone = 0xffffffffu;

// The longest backtick string whose closing string is remembered, which keeps the search for closing strings linear.
constexpr uint32_t kMaxBacktickRun = 1000;

// The classes of delimiter whose openers are searched separately, by character, by whether the closer can open, and by
// its length modulo 3.
constexpr uint32_t kDelimiterClasses = 7 * 2 * 3;

// The characters that smart punctuation writes (PrMarkdownExtensionFlag_SmartPunctuation).
constexpr char kLeftSingleQuote[] = "\xe2\x80\x98";
constexpr char kRightSingleQuote[] = "\xe2\x80\x99";
constexpr char kLeftDoubleQuote[] = "\xe2\x80\x9c";
constexpr char kRightDoubleQuote[] = "\xe2\x80\x9d";
constexpr char kEnDash[] = "\xe2\x80\x93";
constexpr char kEmDash[] = "\xe2\x80\x94";
constexpr char kEllipsis[] = "\xe2\x80\xa6";

// The content of one block as it is parsed.
struct Subject {
    MarkdownInlineParser* parser;
    const PrMarkdownOptions* options;
    MarkdownTree* tree;
    MarkdownNodeIndex parent;
    PrStringView text;
    size_t pos;
    uint32_t lastDelimiter;
    bool noLinkOpeners;             // true once a link closes, until a '[' opens after it

    // True once a search for a closing backtick string has read to the end of the content, after which the parser's
    // backticks tell where the last string of each length starts.
    bool scannedBackticks;
    bool clearedBackticks;          // true once the parser's backticks are zero for this content
    MarkdownTagScan tags;

    // The offsets from which a search found no closing $`, $$ or $.
    size_t missingMath[3];

    MarkdownResolveFn resolve;
    void* context;
};

char peek(const Subject& s, size_t index) {
    return index < s.text.size ? s.text.data[index] : '\0';
}

MarkdownTreeNode& nodeOf(Subject& s, MarkdownNodeIndex node) {
    return s.tree->nodes.data[node];
}

bool isSpecial(const Subject& s, char c) {
    switch (c) {
        case '\n':
        case '\\':
        case '`':
        case '&':
        case '<':
        case '*':
        case '_':
        case '[':
        case ']':
        case '!':
            return true;
        case '~':
            return (s.options->extensions
                    & (PrMarkdownExtensionFlag_Strikethrough | PrMarkdownExtensionFlag_Scripts)) != 0;
        case '^':
            return (s.options->extensions & PrMarkdownExtensionFlag_Scripts) != 0;
        case '=':
            return (s.options->extensions & PrMarkdownExtensionFlag_Highlight) != 0;
        case '$':
            return (s.options->extensions & PrMarkdownExtensionFlag_Math) != 0;
        case 'w':
        case ':':
            return (s.options->extensions & PrMarkdownExtensionFlag_Autolinks) != 0;
        case '\'':
        case '"':
        case '-':
        case '.':
            return (s.options->extensions & PrMarkdownExtensionFlag_SmartPunctuation) != 0;
        default:
            return false;
    }
}

// Creates a node of `kind` as the last child of the block.
MarkdownNodeIndex append(Subject& s, PrMarkdownNodeKind kind) {
    return markdownTreeAppend(s.tree, s.parent, kind);
}

// Creates a Text node of `text` under `parent`.
MarkdownNodeIndex appendText(Subject& s, MarkdownNodeIndex parent, PrStringView text,
                             PrMarkdownTextKind kind = PrMarkdownTextKind_Plain) {
    MarkdownRange range = markdownTreeAddText(s.tree, text);
    MarkdownNodeIndex node = markdownTreeAppend(s.tree, parent, PrMarkdownNodeKind_Text);
    nodeOf(s, node).values.textKind = kind;
    nodeOf(s, node).values.text = range;
    return node;
}

MarkdownNodeIndex appendText(Subject& s, PrStringView text) {
    return appendText(s, s.parent, text);
}

// Appends a Text node to the block whose text is the tree's text from `mark` to its end.
void appendTextFrom(Subject& s, uint32_t mark) {
    MarkdownNodeIndex node = append(s, PrMarkdownNodeKind_Text);
    nodeOf(s, node).values.text = MarkdownRange{mark, s.tree->text.count - mark};
}

void skipSpaces(Subject& s) {
    while (markdownIsSpaceOrTab(peek(s, s.pos)))
        ++s.pos;
}

size_t skipWhitespace(const Subject& s, size_t index) {
    while (markdownIsSpaceOrTab(peek(s, index)) || peek(s, index) == '\n')
        ++index;
    return index;
}

// Returns the code point that ends at `end`, or a line feed at the start of the content.
uint32_t codePointBefore(const Subject& s, size_t end) {
    if (end == 0)
        return '\n';
    size_t start = end - 1;
    while (start > 0 && (uint8_t(s.text.data[start]) & 0xc0) == 0x80)
        --start;
    uint32_t codePoint = 0;
    prUtf8Decode(stringSlice(s.text, start, end - start), 0, &codePoint);
    return codePoint;
}

// Returns the code point at `start`, or a line feed at the end of the content.
uint32_t codePointAt(const Subject& s, size_t start) {
    if (start >= s.text.size)
        return '\n';
    uint32_t codePoint = 0;
    prUtf8Decode(s.text, start, &codePoint);
    return codePoint;
}

// ---------------------------------------------------------------------------------------------------------------------
// Delimiters

void pushDelimiter(Subject& s, char character, bool canOpen, bool canClose, MarkdownNodeIndex node) {
    uint32_t index = s.parser->delimiters.count;
    MarkdownDelimiter& delimiter = s.parser->delimiters.emplace();
    delimiter.node = node;
    delimiter.previous = s.lastDelimiter;
    delimiter.next = kDelimiterNone;
    delimiter.position = uint32_t(s.pos);
    delimiter.length = nodeOf(s, node).values.text.size;
    delimiter.character = character;
    delimiter.canOpen = canOpen;
    delimiter.canClose = canClose;
    if (s.lastDelimiter != kDelimiterNone)
        s.parser->delimiters.data[s.lastDelimiter].next = index;
    s.lastDelimiter = index;
}

void removeDelimiter(Subject& s, uint32_t index) {
    MarkdownDelimiter& delimiter = s.parser->delimiters.data[index];
    if (delimiter.next != kDelimiterNone)
        s.parser->delimiters.data[delimiter.next].previous = delimiter.previous;
    else
        s.lastDelimiter = delimiter.previous;
    if (delimiter.previous != kDelimiterNone)
        s.parser->delimiters.data[delimiter.previous].next = delimiter.next;
}

uint32_t lengthOf(Subject& s, uint32_t delimiter) {
    return nodeOf(s, s.parser->delimiters.data[delimiter].node).values.text.size;
}

// Scans the run of `c` at the subject's position, moves past it, and returns its length and whether it can open and
// close emphasis (commonmark/#left-flanking-delimiter-run).
uint32_t scanDelimiters(Subject& s, char c, bool* canOpen, bool* canClose) {
    uint32_t before = codePointBefore(s, s.pos);
    size_t start = s.pos;
    bool quote = c == '\'' || c == '"';
    while (peek(s, s.pos) == c && !(quote && s.pos > start))
        ++s.pos;
    uint32_t after = codePointAt(s, s.pos);
    bool spaceBefore = markdownIsWhitespace(before);
    bool spaceAfter = markdownIsWhitespace(after);
    bool punctuationBefore = markdownIsPunctuation(before);
    bool punctuationAfter = markdownIsPunctuation(after);
    bool left = !spaceAfter && (!punctuationAfter || spaceBefore || punctuationBefore);
    bool right = !spaceBefore && (!punctuationBefore || spaceAfter || punctuationAfter);
    if (c == '_') {
        *canOpen = left && (!right || punctuationBefore);
        *canClose = right && (!left || punctuationAfter);
    } else if (quote) {
        // A quote after a closing bracket or parenthesis closes, as in "[a]'s".
        *canOpen = left && !right && before != ']' && before != ')';
        *canClose = right;
    } else {
        *canOpen = left;
        *canClose = right;
    }
    return uint32_t(s.pos - start);
}

void handleDelimiter(Subject& s, char c) {
    bool canOpen = false;
    bool canClose = false;
    size_t start = s.pos;
    uint32_t length = scanDelimiters(s, c, &canOpen, &canClose);
    PrStringView characters = stringSlice(s.text, start, length);
    // A quote is written as a closing quote, or as an opening one if it is a double quote that cannot close. A match
    // with another quote makes the pair a left and a right quote.
    if (c == '\'')
        characters = kRightSingleQuote;
    else if (c == '"')
        characters = canClose ? kRightDoubleQuote : kLeftDoubleQuote;
    MarkdownNodeIndex node = appendText(s, characters);
    bool pushes = canOpen || canClose;
    PrMarkdownExtensionFlags extensions = s.options->extensions;
    if (c == '~') {
        // One tilde is a subscript with scripts, and a strikethrough without them.
        bool strikes = (extensions & PrMarkdownExtensionFlag_Strikethrough) != 0;
        bool scripts = (extensions & PrMarkdownExtensionFlag_Scripts) != 0;
        pushes = pushes && ((length == 2 && strikes) || (length == 1 && (strikes || scripts)));
    } else if (c == '^') {
        pushes = pushes && length == 1;
    } else if (c == '=') {
        pushes = pushes && length == 2;
    }
    if (pushes)
        pushDelimiter(s, c, canOpen, canClose, node);
}

// Moves the nodes after `from`, up to `to` or the last sibling if `to` is none, under `wrapper`.
void wrap(Subject& s, MarkdownNodeIndex from, MarkdownNodeIndex to, MarkdownNodeIndex wrapper) {
    MarkdownNodeIndex node = nodeOf(s, from).next;
    while (node != kMarkdownNodeNone && node != to) {
        MarkdownNodeIndex next = nodeOf(s, node).next;
        markdownTreeUnlink(s.tree, node);
        markdownTreeAppendChild(s.tree, wrapper, node);
        node = next;
    }
}

// Makes emphasis or strong emphasis of what lies between `opener` and `closer`, and returns the delimiter at which to
// continue (commonmark/#process-emphasis).
uint32_t insertEmphasis(Subject& s, uint32_t opener, uint32_t closer) {
    MarkdownNodeIndex openerNode = s.parser->delimiters.data[opener].node;
    MarkdownNodeIndex closerNode = s.parser->delimiters.data[closer].node;
    uint32_t used = lengthOf(s, opener) >= 2 && lengthOf(s, closer) >= 2 ? 2 : 1;
    nodeOf(s, openerNode).values.text.size -= used;
    nodeOf(s, closerNode).values.text.size -= used;

    uint32_t between = s.parser->delimiters.data[closer].previous;
    while (between != kDelimiterNone && between != opener) {
        uint32_t previous = s.parser->delimiters.data[between].previous;
        removeDelimiter(s, between);
        between = previous;
    }

    MarkdownNodeIndex emphasis =
        markdownTreeCreate(s.tree, used == 1 ? PrMarkdownNodeKind_Emphasis : PrMarkdownNodeKind_Strong);
    wrap(s, openerNode, closerNode, emphasis);
    markdownTreeInsertAfter(s.tree, openerNode, emphasis);

    if (nodeOf(s, openerNode).values.text.size == 0) {
        markdownTreeUnlink(s.tree, openerNode);
        removeDelimiter(s, opener);
    }
    if (nodeOf(s, closerNode).values.text.size == 0) {
        markdownTreeUnlink(s.tree, closerNode);
        uint32_t next = s.parser->delimiters.data[closer].next;
        removeDelimiter(s, closer);
        return next;
    }
    return closer;
}

// Returns the kind of node that a pair of delimiter runs of `character` and `length` makes, other than emphasis.
PrMarkdownNodeKind pairedKind(const Subject& s, char character, uint32_t length) {
    if (character == '^')
        return PrMarkdownNodeKind_Superscript;
    if (character == '=')
        return PrMarkdownNodeKind_Highlight;
    bool scripts = (s.options->extensions & PrMarkdownExtensionFlag_Scripts) != 0;
    return length == 1 && scripts ? PrMarkdownNodeKind_Subscript : PrMarkdownNodeKind_Strikethrough;
}

// Makes a strikethrough, a highlight, a subscript or a superscript of what lies between `opener` and `closer` if their
// runs are the same length, removes both and every delimiter between them, and returns the delimiter at which to
// continue (gfm/#strikethrough-extension).
uint32_t insertPaired(Subject& s, uint32_t opener, uint32_t closer) {
    uint32_t next = s.parser->delimiters.data[closer].next;
    MarkdownNodeIndex openerNode = s.parser->delimiters.data[opener].node;
    MarkdownNodeIndex closerNode = s.parser->delimiters.data[closer].node;
    uint32_t length = lengthOf(s, opener);
    if (length == lengthOf(s, closer)) {
        PrMarkdownNodeKind kind = pairedKind(s, s.parser->delimiters.data[closer].character, length);
        MarkdownNodeIndex strikethrough = markdownTreeCreate(s.tree, kind);
        wrap(s, openerNode, closerNode, strikethrough);
        markdownTreeInsertAfter(s.tree, openerNode, strikethrough);
        markdownTreeUnlink(s.tree, openerNode);
        markdownTreeUnlink(s.tree, closerNode);
    }
    uint32_t delimiter = closer;
    while (delimiter != kDelimiterNone && delimiter != opener) {
        uint32_t previous = s.parser->delimiters.data[delimiter].previous;
        removeDelimiter(s, delimiter);
        delimiter = previous;
    }
    removeDelimiter(s, opener);
    return next;
}

uint32_t classOf(char character, bool canOpen, uint32_t length) {
    uint32_t base = 0;
    switch (character) {
        case '*':
            base = 0;
            break;
        case '_':
            base = 1;
            break;
        case '~':
            base = 2;
            break;
        case '\'':
            base = 3;
            break;
        case '"':
            base = 4;
            break;
        case '^':
            base = 5;
            break;
        default:
            base = 6;
            break;
    }
    return (base * 2 + (canOpen ? 1 : 0)) * 3 + length % 3;
}

// Resolves the emphasis, the quotes and the other pairs among the delimiters at or after `bottom`
// (commonmark/#process-emphasis), and removes those delimiters.
void processEmphasis(Subject& s, uint32_t bottom) {
    Array<MarkdownDelimiter>& delimiters = s.parser->delimiters;
    uint32_t openersBottom[kDelimiterClasses];
    for (uint32_t i = 0; i < kDelimiterClasses; ++i)
        openersBottom[i] = bottom;

    uint32_t closer = kDelimiterNone;
    for (uint32_t candidate = s.lastDelimiter;
         candidate != kDelimiterNone && delimiters.data[candidate].position >= bottom;
         candidate = delimiters.data[candidate].previous)
        closer = candidate;

    while (closer != kDelimiterNone) {
        MarkdownDelimiter close = delimiters.data[closer];
        if (!close.canClose) {
            closer = close.next;
            continue;
        }
        uint32_t closerLength = close.length;
        uint32_t closerClass = classOf(close.character, close.canOpen, closerLength);
        uint32_t opener = close.previous;
        bool found = false;
        while (opener != kDelimiterNone && delimiters.data[opener].position >= openersBottom[closerClass]) {
            const MarkdownDelimiter& open = delimiters.data[opener];
            if (open.canOpen && open.character == close.character) {
                // If the closer can open or the opener can close, the runs match only if the sum of their lengths is
                // not a multiple of 3, unless both lengths are (commonmark/#can-open-emphasis).
                uint32_t openerLength = open.length;
                if (!(close.canOpen || open.canClose) || closerLength % 3 == 0
                    || (openerLength + closerLength) % 3 != 0) {
                    found = true;
                    break;
                }
            }
            opener = open.previous;
        }
        uint32_t oldCloser = closer;
        if (close.character == '\'' || close.character == '"') {
            // A matched pair of quotes is a left and a right quote. An unmatched one stays as it was written.
            bool single = close.character == '\'';
            PrStringView right = single ? kRightSingleQuote : kRightDoubleQuote;
            nodeOf(s, close.node).values.text = markdownTreeAddText(s.tree, right);
            closer = close.next;
            if (found) {
                MarkdownNodeIndex openerNode = delimiters.data[opener].node;
                nodeOf(s, openerNode).values.text =
                    markdownTreeAddText(s.tree, single ? kLeftSingleQuote : kLeftDoubleQuote);
                removeDelimiter(s, opener);
                removeDelimiter(s, oldCloser);
            }
        } else if (!found) {
            closer = close.next;
        } else if (close.character == '~' || close.character == '^' || close.character == '=') {
            closer = insertPaired(s, opener, closer);
        } else {
            closer = insertEmphasis(s, opener, closer);
        }
        if (!found) {
            openersBottom[closerClass] = delimiters.data[oldCloser].position;
            if (!close.canOpen)
                removeDelimiter(s, oldCloser);
        }
    }
    while (s.lastDelimiter != kDelimiterNone && delimiters.data[s.lastDelimiter].position >= bottom)
        removeDelimiter(s, s.lastDelimiter);
}

// ---------------------------------------------------------------------------------------------------------------------
// Code spans, escapes, references and tags

// Returns the offset after the closing backtick string of `length` that follows `from`, or 0 if there is none.
size_t closingBackticks(Subject& s, size_t from, uint32_t length) {
    Array<uint32_t>& backticks = s.parser->backticks;
    if (!s.clearedBackticks) {
        backticks.resize(kMaxBacktickRun + 1);
        memZero(backticks.data, backticks.count * sizeof(uint32_t));
        s.clearedBackticks = true;
    }
    if (length > kMaxBacktickRun || (s.scannedBackticks && backticks.data[length] <= from))
        return 0;
    size_t index = from;
    while (true) {
        while (index < s.text.size && s.text.data[index] != '`')
            ++index;
        if (index >= s.text.size)
            break;
        size_t start = index;
        while (index < s.text.size && s.text.data[index] == '`')
            ++index;
        size_t run = index - start;
        if (run <= kMaxBacktickRun)
            backticks.data[run] = uint32_t(start);
        if (run == length)
            return index;
    }
    s.scannedBackticks = true;
    return 0;
}

void handleBackticks(Subject& s) {
    size_t start = s.pos;
    while (peek(s, s.pos) == '`')
        ++s.pos;
    uint32_t length = uint32_t(s.pos - start);
    size_t end = closingBackticks(s, s.pos, length);
    if (end == 0) {
        appendText(s, stringSlice(s.text, start, length));
        return;
    }
    // Line endings become spaces, and one space is stripped from each end if both ends have one, unless the code is
    // only spaces (commonmark/#code-spans).
    PrStringView code = stringSlice(s.text, s.pos, end - length - s.pos);
    s.pos = end;
    uint32_t mark = s.tree->text.count;
    for (size_t i = 0; i < code.size; ++i)
        s.tree->text.push(code.data[i] == '\n' ? ' ' : code.data[i]);
    PrStringView written{s.tree->text.data + mark, code.size};
    bool onlySpaces = true;
    for (size_t i = 0; i < written.size; ++i)
        onlySpaces = onlySpaces && written.data[i] == ' ';
    MarkdownRange range{mark, uint32_t(code.size)};
    if (!onlySpaces && written.size >= 2 && written.data[0] == ' ' && written.data[written.size - 1] == ' ')
        range = MarkdownRange{mark + 1, range.size - 2};
    MarkdownNodeIndex node = append(s, PrMarkdownNodeKind_Code);
    MarkdownNodeIndex text = markdownTreeAppend(s.tree, node, PrMarkdownNodeKind_Text);
    nodeOf(s, text).values.text = range;
}

void handleBackslash(Subject& s) {
    char next = peek(s, s.pos + 1);
    if (markdownIsAsciiPunctuation(next)) {
        appendText(s, stringSlice(s.text, s.pos + 1, 1));
        s.pos += 2;
    } else if (next == '\n') {
        s.pos += 2;
        skipSpaces(s);
        append(s, PrMarkdownNodeKind_HardBreak);
    } else {
        appendText(s, "\\");
        ++s.pos;
    }
}

void handleReference(Subject& s) {
    uint32_t mark = s.tree->text.count;
    size_t length = markdownDecodeReference(s.text, s.pos, &s.tree->text);
    if (length == 0) {
        appendText(s, "&");
        ++s.pos;
        return;
    }
    appendTextFrom(s, mark);
    s.pos += length;
}

// Appends `text` to the tree's text with its character references decoded, and returns its range.
MarkdownRange addWithReferences(Subject& s, PrStringView text) {
    uint32_t mark = s.tree->text.count;
    size_t run = 0;
    size_t index = 0;
    while (index < text.size) {
        if (text.data[index] == '&') {
            s.tree->text.append(text.data + run, uint32_t(index - run));
            size_t length = markdownDecodeReference(text, index, &s.tree->text);
            if (length != 0) {
                index += length;
                run = index;
                continue;
            }
            s.tree->text.push('&');
            run = ++index;
            continue;
        }
        ++index;
    }
    s.tree->text.append(text.data + run, uint32_t(text.size - run));
    return MarkdownRange{mark, s.tree->text.count - mark};
}

void appendRawHtml(Subject& s, PrStringView html) {
    switch (s.options->rawHtml) {
        case PrMarkdownRawHtml_Pass:
            appendText(s, s.parent, html, PrMarkdownTextKind_Html);
            break;
        case PrMarkdownRawHtml_Filter: {
            uint32_t mark = s.tree->text.count;
            markdownAppendFilteredHtml(html, &s.tree->text);
            appendTextFrom(s, mark);
            nodeOf(s, s.tree->nodes.count - 1).values.textKind = PrMarkdownTextKind_Html;
            break;
        }
        case PrMarkdownRawHtml_Escape:
            appendText(s, html);
            break;
    }
}

// Returns `destination`, or an empty destination if the options keep only safe destinations and it is not safe.
MarkdownRange checkedDestination(Subject& s, MarkdownRange destination) {
    if (s.options->destinations == PrMarkdownDestinations_Safe
        && markdownIsUnsafeDestination(markdownTreeText(s.tree, destination)))
        return MarkdownRange{destination.start, 0};
    return destination;
}

void handleLessThan(Subject& s) {
    size_t length = markdownScanUriAutolink(s.text, s.pos);
    bool email = false;
    if (length == 0) {
        length = markdownScanEmailAutolink(s.text, s.pos);
        email = length != 0;
    }
    if (length != 0) {
        PrStringView address = stringSlice(s.text, s.pos + 1, length - 2);
        s.pos += length;
        uint32_t mark = s.tree->text.count;
        if (email)
            s.tree->text.append("mailto:", 7);
        addWithReferences(s, address);
        MarkdownRange destination{mark, s.tree->text.count - mark};
        MarkdownNodeIndex link = append(s, PrMarkdownNodeKind_Link);
        nodeOf(s, link).values.destination = checkedDestination(s, destination);
        MarkdownNodeIndex text = markdownTreeAppend(s.tree, link, PrMarkdownNodeKind_Text);
        nodeOf(s, text).values.text = MarkdownRange{mark + (email ? 7 : 0), destination.size - (email ? 7 : 0)};
        return;
    }
    length = markdownScanHtmlTag(s.text, s.pos, &s.tags);
    if (length != 0) {
        appendRawHtml(s, stringSlice(s.text, s.pos, length));
        s.pos += length;
        return;
    }
    appendText(s, "<");
    ++s.pos;
}

// Writes a run of hyphens as dashes: em dashes if the run divides by 3, en dashes if it divides by 2, and otherwise em
// dashes followed by one or two en dashes. A single hyphen stays a hyphen.
void handleHyphens(Subject& s) {
    size_t start = s.pos;
    while (peek(s, s.pos) == '-')
        ++s.pos;
    size_t hyphens = s.pos - start;
    if (hyphens == 1) {
        appendText(s, "-");
        return;
    }
    size_t em = 0;
    size_t en = 0;
    if (hyphens % 3 == 0) {
        em = hyphens / 3;
    } else if (hyphens % 2 == 0) {
        en = hyphens / 2;
    } else if (hyphens % 3 == 2) {
        en = 1;
        em = (hyphens - 2) / 3;
    } else {
        en = 2;
        em = (hyphens - 4) / 3;
    }
    uint32_t mark = s.tree->text.count;
    for (size_t i = 0; i < em; ++i)
        s.tree->text.append(kEmDash, 3);
    for (size_t i = 0; i < en; ++i)
        s.tree->text.append(kEnDash, 3);
    appendTextFrom(s, mark);
}

// Writes "..." as an ellipsis.
void handlePeriod(Subject& s) {
    if (peek(s, s.pos + 1) == '.' && peek(s, s.pos + 2) == '.') {
        appendText(s, kEllipsis);
        s.pos += 3;
    } else if (peek(s, s.pos + 1) == '.') {
        appendText(s, "..");
        s.pos += 2;
    } else {
        appendText(s, ".");
        ++s.pos;
    }
}

// Appends a Math node of `tex` to the block, and moves the subject's position to `end`.
void appendMath(Subject& s, PrStringView tex, bool display, size_t end) {
    MarkdownNodeIndex math = append(s, PrMarkdownNodeKind_Math);
    nodeOf(s, math).values.display = display;
    appendText(s, math, tex);
    s.pos = end;
}

// Returns the offset of the first `needle` at or after `from` that `closes` accepts, or the content's size if there is
// none. The search does not look again past an offset where it found none before, which `*missingAfter` records.
template <typename Closes>
size_t findCloser(Subject& s, size_t from, PrStringView needle, size_t* missingAfter, Closes closes) {
    if (from >= *missingAfter)
        return s.text.size;
    for (size_t at = from; at + needle.size <= s.text.size; ++at) {
        if (stringStartsWith(stringSlice(s.text, at, s.text.size - at), needle) && closes(at))
            return at;
    }
    *missingAfter = from;
    return s.text.size;
}

// Matches the math at the subject's position, which is a '$': TeX in $`…`$, between $$, or between single dollar signs
// that hold no whitespace at their inner sides, and whose closing sign is not followed by a digit, as "$5 and $6" is
// not math.
void handleDollar(Subject& s) {
    size_t start = s.pos;
    if (peek(s, start + 1) == '`') {
        size_t end = findCloser(s, start + 2, "`$", &s.missingMath[0], [](size_t) { return true; });
        if (end < s.text.size && end > start + 2) {
            appendMath(s, stringSlice(s.text, start + 2, end - start - 2), false, end + 2);
            return;
        }
    } else if (peek(s, start + 1) == '$') {
        size_t end = findCloser(s, start + 2, "$$", &s.missingMath[1],
                                [&](size_t at) { return at == 0 || s.text.data[at - 1] != '\\'; });
        if (end < s.text.size && end > start + 2) {
            appendMath(s, stringSlice(s.text, start + 2, end - start - 2), true, end + 2);
            return;
        }
        appendText(s, "$$");
        s.pos = start + 2;
        return;
    } else if (!markdownIsSpaceOrTab(peek(s, start + 1)) && peek(s, start + 1) != '\n' && start + 1 < s.text.size) {
        size_t end = findCloser(s, start + 1, "$", &s.missingMath[2], [&](size_t at) {
            char before = s.text.data[at - 1];
            return at > start + 1 && before != '\\' && !markdownIsSpaceOrTab(before) && before != '\n'
                && !asciiIsDigit(peek(s, at + 1));
        });
        if (end < s.text.size) {
            appendMath(s, stringSlice(s.text, start + 1, end - start - 1), false, end + 1);
            return;
        }
    }
    appendText(s, "$");
    s.pos = start + 1;
}

void handleNewline(Subject& s) {
    size_t newline = s.pos;
    ++s.pos;
    skipSpaces(s);
    bool hard = newline >= 2 && s.text.data[newline - 1] == ' ' && s.text.data[newline - 2] == ' ';
    append(s, hard ? PrMarkdownNodeKind_HardBreak : PrMarkdownNodeKind_SoftBreak);
}

// ---------------------------------------------------------------------------------------------------------------------
// Links and images

void pushBracket(Subject& s, bool image, MarkdownNodeIndex node) {
    Array<MarkdownBracket>& brackets = s.parser->brackets;
    if (brackets.count != 0)
        brackets.last().bracketAfter = true;
    MarkdownBracket& bracket = brackets.emplace();
    bracket.node = node;
    bracket.position = uint32_t(s.pos);
    bracket.image = image;
    bracket.bracketAfter = false;
    if (!image)
        s.noLinkOpeners = false;
}

// Scans the label of a full or collapsed reference at `start`, after a link's text. Unlike a definition's label, it may
// be empty. Returns the offset after its ']', or 0 if there is none, and writes its characters within its brackets to
// `out`.
size_t referenceLabel(const Subject& s, size_t start, MarkdownSpan* out) {
    if (peek(s, start) != '[')
        return 0;
    size_t index = start + 1;
    while (index < s.text.size && s.text.data[index] != '[' && s.text.data[index] != ']') {
        if (s.text.data[index] == '\\' && markdownIsAsciiPunctuation(peek(s, index + 1)))
            ++index;
        ++index;
        if (index - start - 1 > kMarkdownMaxLabelBytes)
            return 0;
    }
    if (peek(s, index) != ']')
        return 0;
    *out = MarkdownSpan{start + 1, index - start - 1};
    return index + 1;
}

// Looks up the reference `label`, and writes its destination and title to the tree's text if a definition matches it.
bool resolveReference(Subject& s, PrStringView label, MarkdownRange* outDestination, MarkdownRange* outTitle) {
    if (label.size == 0 || label.size > kMarkdownMaxLabelBytes)
        return false;
    s.parser->label.clear();
    if (!markdownAppendNormalizedLabel(label, &s.parser->label))
        return false;
    MarkdownDefinition definition{};
    PrStringView normalized{s.parser->label.data, s.parser->label.count};
    if (!s.resolve(s.context, normalized, &definition))
        return false;
    *outDestination = markdownTreeAddText(s.tree, definition.destination);
    *outTitle = markdownTreeAddText(s.tree, definition.title);
    return true;
}

// Tries the inline link that may follow a link's text at `start`, which is its '('. Returns the offset after its ')',
// or 0 if there is none (commonmark/#inline-link).
size_t inlineLink(Subject& s, size_t start, MarkdownRange* outDestination, MarkdownRange* outTitle) {
    if (peek(s, start) != '(')
        return 0;
    size_t destinationStart = skipWhitespace(s, start + 1);
    size_t destinationLength = 0;
    MarkdownSpan destination{};
    if (!markdownScanLinkDestination(s.text, destinationStart, &destinationLength, &destination))
        return 0;
    size_t destinationEnd = destinationStart + destinationLength;
    size_t titleStart = skipWhitespace(s, destinationEnd);
    MarkdownSpan title{};
    size_t titleEnd = titleStart;
    if (titleStart != destinationEnd)
        titleEnd += markdownScanLinkTitle(s.text, titleStart, &title);
    size_t end = skipWhitespace(s, titleEnd);
    if (peek(s, end) != ')')
        return 0;
    uint32_t mark = s.tree->text.count;
    markdownAppendDecoded(stringSlice(s.text, destination.start, destination.size), &s.tree->text);
    *outDestination = MarkdownRange{mark, s.tree->text.count - mark};
    mark = s.tree->text.count;
    if (titleEnd != titleStart)
        markdownAppendDecoded(stringSlice(s.text, title.start, title.size), &s.tree->text);
    *outTitle = MarkdownRange{mark, s.tree->text.count - mark};
    return end + 1;
}

// Makes the brackets that `opener` opened, and that close before `initial`, a footnote reference if they hold "^" and
// more. The reference takes the text between them as it is written, and whether a definition has its label does not
// matter here. Returns true if it made one.
bool footnoteReference(Subject& s, const MarkdownBracket& opener, size_t initial) {
    if ((s.options->extensions & PrMarkdownExtensionFlag_Footnotes) == 0)
        return false;
    MarkdownNodeIndex first = nodeOf(s, opener.node).next;
    if (first == kMarkdownNodeNone || nodeOf(s, first).values.kind != PrMarkdownNodeKind_Text
        || nodeOf(s, first).values.textKind != PrMarkdownTextKind_Plain)
        return false;
    MarkdownRange caret = nodeOf(s, first).values.text;
    if (caret.size == 0 || s.tree->text.data[caret.start] != '^'
        || (caret.size == 1 && nodeOf(s, first).next == kMarkdownNodeNone))
        return false;
    s.pos = initial;
    // The reference's text is "[^label]", which the walk shows if no definition has the label.
    size_t labelStart = opener.position + 1;
    PrStringView label = stringSlice(s.text, labelStart, initial - 1 > labelStart ? initial - 1 - labelStart : 0);
    uint32_t mark = s.tree->text.count;
    s.tree->text.append("[^", 2);
    s.tree->text.append(label.data, uint32_t(label.size));
    s.tree->text.push(']');
    MarkdownNodeIndex reference = markdownTreeCreate(s.tree, PrMarkdownNodeKind_FootnoteReference);
    nodeOf(s, reference).values.text = MarkdownRange{mark, s.tree->text.count - mark};
    nodeOf(s, reference).values.info = MarkdownRange{mark + 2, uint32_t(label.size)};
    markdownTreeInsertBefore(s.tree, opener.node, reference);
    processEmphasis(s, opener.position);
    while (nodeOf(s, opener.node).next != kMarkdownNodeNone)
        markdownTreeUnlink(s.tree, nodeOf(s, opener.node).next);
    markdownTreeUnlink(s.tree, opener.node);
    s.parser->brackets.pop();
    return true;
}

void handleCloseBracket(Subject& s) {
    ++s.pos;
    size_t initial = s.pos;
    Array<MarkdownBracket>& brackets = s.parser->brackets;
    if (brackets.count == 0) {
        appendText(s, "]");
        return;
    }
    MarkdownBracket opener = brackets.last();
    if (!opener.image && s.noLinkOpeners) {
        brackets.pop();
        appendText(s, "]");
        return;
    }

    MarkdownRange destination{};
    MarkdownRange title{};
    size_t end = inlineLink(s, initial, &destination, &title);
    bool matched = end != 0;
    if (!matched) {
        // A full reference, or a collapsed or shortcut reference whose label is the link's text
        // (commonmark/#reference-link).
        MarkdownSpan label{};
        size_t after = referenceLabel(s, initial, &label);
        PrStringView text = stringSlice(s.text, label.start, label.size);
        bool hasLabel = after != 0 && stringTrim(text).size != 0;
        if (!hasLabel && !opener.bracketAfter) {
            text = stringSlice(s.text, opener.position, initial - 1 - opener.position);
            hasLabel = true;
            if (after == 0)
                after = initial;
        }
        if (hasLabel && resolveReference(s, text, &destination, &title)) {
            matched = true;
            end = after;
        }
    }
    if (!matched && footnoteReference(s, opener, initial))
        return;
    if (!matched) {
        brackets.pop();
        s.pos = initial;
        appendText(s, "]");
        return;
    }

    s.pos = end;
    PrMarkdownNodeKind kind = opener.image ? PrMarkdownNodeKind_Image : PrMarkdownNodeKind_Link;
    MarkdownNodeIndex link = markdownTreeCreate(s.tree, kind);
    nodeOf(s, link).values.destination = checkedDestination(s, destination);
    nodeOf(s, link).values.title = title;
    markdownTreeInsertBefore(s.tree, opener.node, link);
    wrap(s, opener.node, kMarkdownNodeNone, link);
    markdownTreeUnlink(s.tree, opener.node);
    processEmphasis(s, opener.position);
    brackets.pop();
    // Links do not contain links, so the brackets before this one no longer open links (commonmark/#link-text).
    if (!opener.image)
        s.noLinkOpeners = true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Extended autolinks (gfm/#autolinks-extension)

bool isAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

// Returns true if the character at `index` in `text` may be part of a host name: neither whitespace, nor ASCII
// punctuation, nor of General_Category P. Symbols are host characters, unlike the punctuation that flanks emphasis. A
// byte inside a code point's encoding is not.
bool isHostCharacter(PrStringView text, size_t index) {
    uint8_t lead = uint8_t(text.data[index]);
    if ((lead & 0xc0) == 0x80)
        return false;
    uint32_t codePoint = 0;
    prUtf8Decode(text, index, &codePoint);
    if (codePoint == kPrUtf8Replacement && lead >= 0x80 && lead != 0xef)
        return false;
    if (codePoint < 0x80)
        return !isAsciiSpace(char(codePoint)) && !markdownIsAsciiPunctuation(char(codePoint));
    if (markdownIsWhitespace(codePoint))
        return false;
    switch (prUnicodeGeneralCategory(codePoint)) {
        case PrUnicodeGeneralCategory_ConnectorPunctuation:
        case PrUnicodeGeneralCategory_DashPunctuation:
        case PrUnicodeGeneralCategory_OpenPunctuation:
        case PrUnicodeGeneralCategory_ClosePunctuation:
        case PrUnicodeGeneralCategory_InitialPunctuation:
        case PrUnicodeGeneralCategory_FinalPunctuation:
        case PrUnicodeGeneralCategory_OtherPunctuation:
            return false;
        default:
            return true;
    }
}

// Returns the length of the domain at the start of `text`, or 0 if it is not one: a dot is needed unless `allowShort`
// is set, and an underscore may not be in the last two of its labels (gfm/#valid-domain).
size_t validDomain(PrStringView text, bool allowShort) {
    size_t dots = 0;
    size_t underscoresBefore = 0;
    size_t underscores = 0;
    size_t i = 1;
    for (; i + 1 < text.size; ++i) {
        if (text.data[i] == '\\' && i + 2 < text.size)
            ++i;
        if (text.data[i] == '_') {
            ++underscores;
        } else if (text.data[i] == '.') {
            underscoresBefore = underscores;
            underscores = 0;
            ++dots;
        } else if (!isHostCharacter(text, i) && text.data[i] != '-') {
            break;
        }
    }
    // A domain of many labels is taken despite its underscores, which keeps the scan linear.
    if ((underscoresBefore > 0 || underscores > 0) && dots <= 10)
        return 0;
    if (allowShort)
        return i;
    return dots != 0 ? i : 0;
}

// Returns the length of the autolink that `text` starts with, at most `end`, once the punctuation that ends a sentence
// and unbalanced parentheses are taken off its end (gfm/#extended-autolink-path-validation).
size_t trimAutolink(PrStringView text, size_t end) {
    size_t opening = 0;
    size_t closing = 0;
    for (size_t i = 0; i < end; ++i) {
        char c = text.data[i];
        if (c == '<') {
            end = i;
            break;
        }
        if (c == '(')
            ++opening;
        else if (c == ')')
            ++closing;
    }
    while (end > 0) {
        char c = text.data[end - 1];
        switch (c) {
            case ')':
                if (closing <= opening)
                    return end;
                --closing;
                --end;
                break;
            case '?':
            case '!':
            case '.':
            case ',':
            case ':':
            case '*':
            case '_':
            case '~':
            case '\'':
            case '"':
                --end;
                break;
            case ';': {
                // An entity reference at the end is not part of the link.
                if (end < 2) {
                    --end;
                    break;
                }
                size_t start = end - 2;
                while (start > 0 && asciiIsLetter(text.data[start]))
                    --start;
                if (start < end - 2 && text.data[start] == '&')
                    end = start;
                else
                    --end;
                break;
            }
            default:
                return end;
        }
    }
    return end;
}

// Appends a link whose text is `text`, and whose destination is `prefix` followed by `text`.
void appendAutolink(Subject& s, PrStringView prefix, PrStringView text) {
    uint32_t mark = s.tree->text.count;
    s.tree->text.append(prefix.data, uint32_t(prefix.size));
    s.tree->text.append(text.data, uint32_t(text.size));
    MarkdownNodeIndex link = append(s, PrMarkdownNodeKind_Link);
    nodeOf(s, link).values.destination =
        checkedDestination(s, MarkdownRange{mark, s.tree->text.count - mark});
    MarkdownNodeIndex child = markdownTreeAppend(s.tree, link, PrMarkdownNodeKind_Text);
    nodeOf(s, child).values.text = MarkdownRange{mark + uint32_t(prefix.size), uint32_t(text.size)};
}

// Matches an extended www autolink at the subject's position, which is a 'w' (gfm/#extended-www-autolink).
bool wwwAutolink(Subject& s) {
    if (s.pos > 0) {
        char before = s.text.data[s.pos - 1];
        if (before != '*' && before != '_' && before != '~' && before != '(' && !isAsciiSpace(before))
            return false;
    }
    PrStringView rest = stringSlice(s.text, s.pos, s.text.size - s.pos);
    if (rest.size < 4 || !stringStartsWith(rest, "www."))
        return false;
    size_t end = validDomain(rest, false);
    if (end == 0)
        return false;
    while (end < rest.size && !isAsciiSpace(rest.data[end]) && rest.data[end] != '<')
        ++end;
    end = trimAutolink(rest, end);
    if (end == 0)
        return false;
    appendAutolink(s, "http://", stringSlice(rest, 0, end));
    s.pos += end;
    return true;
}

// Removes `count` characters from the end of the Text nodes at the end of the block.
void unput(Subject& s, size_t count) {
    MarkdownNodeIndex node = nodeOf(s, s.parent).last;
    while (count > 0 && node != kMarkdownNodeNone && nodeOf(s, node).values.kind == PrMarkdownNodeKind_Text) {
        MarkdownRange& text = nodeOf(s, node).values.text;
        if (text.size < count) {
            count -= text.size;
            text.size = 0;
        } else {
            text.size -= uint32_t(count);
            count = 0;
        }
        node = nodeOf(s, node).previous;
    }
}

// Matches an extended URL autolink whose scheme ends at the subject's position, which is a ':'
// (gfm/#extended-url-autolink). The scheme's letters are text already, which the link takes back.
bool urlAutolink(Subject& s) {
    PrStringView rest = stringSlice(s.text, s.pos, s.text.size - s.pos);
    if (rest.size < 4 || rest.data[1] != '/' || rest.data[2] != '/')
        return false;
    size_t rewind = 0;
    while (rewind < s.pos && asciiIsLetter(s.text.data[s.pos - rewind - 1]))
        ++rewind;
    PrStringView url = stringSlice(s.text, s.pos - rewind, rest.size + rewind);
    static const char* const kSchemes[] = {"http://", "https://", "ftp://"};
    bool safe = false;
    for (const char* scheme : kSchemes) {
        size_t length = PrStringView(scheme).size;
        if (url.size > length && asciiStartsWithFold(url, scheme) && isHostCharacter(url, length))
            safe = true;
    }
    if (!safe)
        return false;
    size_t domain = validDomain(stringSlice(rest, 3, rest.size - 3), true);
    if (domain == 0)
        return false;
    size_t end = 3 + domain;
    while (end < rest.size && !isAsciiSpace(rest.data[end]) && rest.data[end] != '<')
        ++end;
    end = trimAutolink(rest, end);
    if (end == 0)
        return false;
    unput(s, rewind);
    appendAutolink(s, "", stringSlice(s.text, s.pos - rewind, end + rewind));
    s.pos += end;
    return true;
}

// Returns true if `protocol` ends at `end` in `text`, starts at or after `start`, and either starts at `start` or
// follows a character that is not a letter or a digit.
bool endsWithProtocol(PrStringView text, size_t end, PrStringView protocol, size_t start) {
    if (protocol.size > end - start
        || !stringStartsWith(stringSlice(text, end - protocol.size, protocol.size), protocol))
        return false;
    if (protocol.size == end - start)
        return true;
    char before = text.data[end - protocol.size - 1];
    return !(asciiIsLetter(before) || asciiIsDigit(before));
}

// Finds the extended email autolinks in the Text node `node` and splits the node around each of them
// (gfm/#extended-email-autolink).
void emailAutolinks(Subject& s, MarkdownNodeIndex node) {
    size_t offset = 0;
    while (true) {
        MarkdownRange range = nodeOf(s, node).values.text;
        PrStringView text = markdownTreeText(s.tree, range);
        if (offset >= text.size)
            return;
        size_t at = offset + stringFind(stringSlice(text, offset, text.size - offset), '@');
        if (at >= text.size)
            return;
        size_t maxRewind = at - offset;
        bool mailto = true;
        bool xmpp = false;
        size_t dots = 0;
        size_t rewind = 0;
        size_t end = 0;
        // A second '@' in the domain restarts the scan from it, with what the first scan found.
        bool restart = true;
        while (restart) {
            restart = false;
            size_t base = offset + maxRewind;
            for (rewind = 0; rewind < maxRewind; ++rewind) {
                char c = text.data[base - rewind - 1];
                if (asciiIsLetter(c) || asciiIsDigit(c) || c == '.' || c == '+' || c == '-' || c == '_')
                    continue;
                if (c == ':' && endsWithProtocol(text, base - rewind, "mailto:", offset)) {
                    mailto = false;
                    continue;
                }
                if (c == ':' && endsWithProtocol(text, base - rewind, "xmpp:", offset)) {
                    mailto = false;
                    xmpp = true;
                    continue;
                }
                break;
            }
            if (rewind == 0)
                break;
            for (end = 1; base + end < text.size; ++end) {
                char c = text.data[base + end];
                if (asciiIsLetter(c) || asciiIsDigit(c))
                    continue;
                if (c == '@') {
                    offset += maxRewind + 1;
                    maxRewind = end - 1;
                    restart = true;
                    break;
                }
                if (c == '.' && base + end + 1 < text.size
                    && (asciiIsLetter(text.data[base + end + 1]) || asciiIsDigit(text.data[base + end + 1])))
                    ++dots;
                else if (c == '/' && xmpp)
                    continue;
                else if (c != '-' && c != '_')
                    break;
            }
        }
        size_t base = offset + maxRewind;
        if (rewind == 0) {
            offset = base + 1;
            continue;
        }
        char last = text.data[base + end - 1];
        if (end < 2 || dots == 0 || (!asciiIsLetter(last) && last != '.')) {
            offset = base + end;
            continue;
        }
        end = trimAutolink(stringSlice(text, base, text.size - base), end);
        if (end == 0) {
            offset = base + 1;
            continue;
        }
        size_t linkStart = base - rewind;
        size_t linkEnd = base + end;

        // The text before the link stays in `node`, and the text after it is a new node, which is searched next.
        MarkdownNodeIndex link = markdownTreeCreate(s.tree, PrMarkdownNodeKind_Link);
        uint32_t mark = s.tree->text.count;
        if (mailto)
            s.tree->text.append("mailto:", 7);
        markdownTreeCopyText(s.tree, MarkdownRange{range.start + uint32_t(linkStart), uint32_t(linkEnd - linkStart)});
        nodeOf(s, link).values.destination = checkedDestination(s, MarkdownRange{mark, s.tree->text.count - mark});
        markdownTreeInsertAfter(s.tree, node, link);
        MarkdownNodeIndex linkText = markdownTreeAppend(s.tree, link, PrMarkdownNodeKind_Text);
        nodeOf(s, linkText).values.text =
            MarkdownRange{range.start + uint32_t(linkStart), uint32_t(linkEnd - linkStart)};
        MarkdownNodeIndex after = markdownTreeCreate(s.tree, PrMarkdownNodeKind_Text);
        nodeOf(s, after).values.text = MarkdownRange{range.start + uint32_t(linkEnd), range.size - uint32_t(linkEnd)};
        markdownTreeInsertAfter(s.tree, link, after);
        nodeOf(s, node).values.text.size = uint32_t(linkStart);
        node = after;
        offset = 0;
    }
}

// Joins each run of adjacent plain Text nodes into one, and finds the email autolinks in it, in the inlines under
// `parent` that are not in a link, code or math.
void postprocessAutolinks(Subject& s, MarkdownNodeIndex parent) {
    Array<MarkdownNodeIndex> pending;
    pending.push(parent);
    while (pending.count != 0) {
        MarkdownNodeIndex container = pending.last();
        pending.pop();
        MarkdownNodeIndex node = nodeOf(s, container).first;
        while (node != kMarkdownNodeNone) {
            MarkdownTreeNode& current = nodeOf(s, node);
            MarkdownNodeIndex next = current.next;
            // A link's text is not linked again, and the text of code and math is literal.
            PrMarkdownNodeKind kind = current.values.kind;
            if (kind == PrMarkdownNodeKind_Link || kind == PrMarkdownNodeKind_Code || kind == PrMarkdownNodeKind_Math) {
                node = next;
                continue;
            }
            if (current.values.kind != PrMarkdownNodeKind_Text || current.values.textKind != PrMarkdownTextKind_Plain) {
                if (current.first != kMarkdownNodeNone)
                    pending.push(node);
                node = next;
                continue;
            }
            bool joins = next != kMarkdownNodeNone && nodeOf(s, next).values.kind == PrMarkdownNodeKind_Text
                && nodeOf(s, next).values.textKind == PrMarkdownTextKind_Plain;
            if (joins) {
                uint32_t mark = s.tree->text.count;
                MarkdownNodeIndex run = node;
                while (run != kMarkdownNodeNone && nodeOf(s, run).values.kind == PrMarkdownNodeKind_Text
                       && nodeOf(s, run).values.textKind == PrMarkdownTextKind_Plain) {
                    markdownTreeCopyText(s.tree, nodeOf(s, run).values.text);
                    MarkdownNodeIndex after = nodeOf(s, run).next;
                    if (run != node)
                        markdownTreeUnlink(s.tree, run);
                    run = after;
                }
                nodeOf(s, node).values.text = MarkdownRange{mark, s.tree->text.count - mark};
                next = run;
            }
            emailAutolinks(s, node);
            node = next;
        }
    }
    pending.reset();
}

} // namespace

void markdownInlineParserReset(MarkdownInlineParser* parser) {
    parser->delimiters.reset();
    parser->brackets.reset();
    parser->label.reset();
    parser->backticks.reset();
}

void markdownParseInlines(MarkdownInlineParser* parser, const PrMarkdownOptions& options, MarkdownTree* tree,
                          MarkdownNodeIndex parent, PrStringView content, MarkdownResolveFn resolve,
                          void* PR_NULLABLE context) {
    Subject s;
    s.parser = parser;
    s.options = &options;
    s.tree = tree;
    s.parent = parent;
    // The end of the content is trimmed. Its lines begin at their first non-whitespace character already.
    size_t size = content.size;
    while (size != 0 && (markdownIsSpaceOrTab(content.data[size - 1]) || content.data[size - 1] == '\n'))
        --size;
    s.text = PrStringView{content.data, size};
    s.pos = 0;
    s.lastDelimiter = kDelimiterNone;
    s.noLinkOpeners = false;
    s.scannedBackticks = false;
    s.clearedBackticks = false;
    s.tags = markdownTagScanStart();
    for (size_t& missing : s.missingMath)
        missing = ~size_t(0);
    s.resolve = resolve;
    s.context = context;
    parser->delimiters.clear();
    parser->brackets.clear();

    while (s.pos < s.text.size) {
        char c = s.text.data[s.pos];
        if (c == '\n') {
            handleNewline(s);
        } else if (c == '`') {
            handleBackticks(s);
        } else if (c == '\\') {
            handleBackslash(s);
        } else if (c == '&') {
            handleReference(s);
        } else if (c == '<') {
            handleLessThan(s);
        } else if (c == '$' && isSpecial(s, c)) {
            handleDollar(s);
        } else if (c == '*' || c == '_'
                   || ((c == '~' || c == '\'' || c == '"' || c == '^' || c == '=') && isSpecial(s, c))) {
            handleDelimiter(s, c);
        } else if (c == '-' && isSpecial(s, c)) {
            handleHyphens(s);
        } else if (c == '.' && isSpecial(s, c)) {
            handlePeriod(s);
        } else if (c == '[') {
            MarkdownNodeIndex node = appendText(s, "[");
            ++s.pos;
            pushBracket(s, false, node);
        } else if (c == '!' && peek(s, s.pos + 1) == '['
                   && !((options.extensions & PrMarkdownExtensionFlag_Footnotes) != 0 && peek(s, s.pos + 2) == '^')) {
            // With footnotes, "![^" is a '!' before a footnote reference rather than the start of an image.
            MarkdownNodeIndex node = appendText(s, "![");
            s.pos += 2;
            pushBracket(s, true, node);
        } else if (c == ']') {
            handleCloseBracket(s);
        } else if ((c == 'w' || c == ':') && isSpecial(s, c) && parser->brackets.count == 0
                   && (c == 'w' ? wwwAutolink(s) : urlAutolink(s))) {
            // The autolink is appended. None is looked for within brackets, which may become a link's text.
        } else {
            size_t end = s.pos + 1;
            while (end < s.text.size && !isSpecial(s, s.text.data[end]))
                ++end;
            PrStringView run = stringSlice(s.text, s.pos, end - s.pos);
            // Spaces at the end of a line are not part of its text (commonmark/#soft-line-breaks).
            if (end < s.text.size && s.text.data[end] == '\n') {
                while (run.size != 0 && markdownIsSpaceOrTab(run.data[run.size - 1]))
                    --run.size;
            }
            if (run.size != 0)
                appendText(s, run);
            s.pos = end;
        }
    }
    processEmphasis(s, 0);
    if ((options.extensions & PrMarkdownExtensionFlag_Autolinks) != 0)
        postprocessAutolinks(s, parent);
}

} // namespace pr_impl
