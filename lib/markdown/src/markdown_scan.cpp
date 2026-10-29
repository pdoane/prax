// Markdown's lexical rules, which the block and inline parsers share.

#include "markdown/src/markdown_scan.h"

#include "pr/ascii.h"
#include "pr/count_of.h"
#include "pr/html_entity.h"
#include "pr/string_ops.h"
#include "pr/unicode.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// The names of the tags whose HTML block, of kind 6, ends at a blank line (commonmark/#html-blocks).
const char* const kBlockTagNames[] = {
    "address", "article", "aside", "base", "basefont", "blockquote", "body", "caption", "center", "col", "colgroup",
    "dd", "details", "dialog", "dir", "div", "dl", "dt", "fieldset", "figcaption", "figure", "footer", "form", "frame",
    "frameset", "h1", "h2", "h3", "h4", "h5", "h6", "head", "header", "hr", "html", "iframe", "legend", "li", "link",
    "main", "menu", "menuitem", "nav", "noframes", "ol", "optgroup", "option", "p", "param", "search", "section",
    "summary", "table", "tbody", "td", "tfoot", "th", "thead", "title", "tr", "track", "ul",
};

// The names of the tags whose HTML block, of kind 1, holds raw text and ends at their end tag.
const char* const kRawTagNames[] = {"pre", "script", "style", "textarea"};

// The most digits in a decimal and a hexadecimal numeric character reference.
constexpr size_t kMaxDecimalDigits = 7;
constexpr size_t kMaxHexDigits = 6;

// The deepest nesting of unescaped parentheses in a link destination. The specification asks for at least three levels,
// and the limit keeps a scan of unbalanced parentheses linear.
constexpr uint32_t kMaxDestinationParentheses = 32;

char at(PrStringView text, size_t index) {
    return index < text.size ? text.data[index] : '\0';
}

bool isLineEnd(PrStringView text, size_t index) {
    return index >= text.size || text.data[index] == '\n' || text.data[index] == '\r';
}

bool isAsciiControl(char c) {
    return uint8_t(c) < 0x20 || c == 0x7f;
}

bool isTagNameChar(char c) {
    return asciiIsLetter(c) || asciiIsDigit(c) || c == '-';
}

// Returns the length of the tag name at `start`, or 0 if none starts there.
size_t tagName(PrStringView text, size_t start) {
    if (!asciiIsLetter(at(text, start)))
        return 0;
    size_t end = start + 1;
    while (isTagNameChar(at(text, end)))
        ++end;
    return end - start;
}

// Returns true if `name` is one of the `count` names at `names`, compared without regard to ASCII case.
bool isOneOf(PrStringView name, const char* const* names, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (asciiEqualsFold(name, names[i]))
            return true;
    }
    return false;
}

// Returns the index after the spaces, tabs and line endings at `start`, which may include any number of line endings.
// Raw HTML is scanned within a paragraph, which holds no blank line, so its whitespace holds at most one line ending.
size_t skipWhitespace(PrStringView text, size_t start) {
    size_t end = start;
    while (end < text.size && (markdownIsSpaceOrTab(text.data[end]) || text.data[end] == '\n' || text.data[end] == '\r'))
        ++end;
    return end;
}

// The ends that a scan for HTML tags searches for, by their position in MarkdownTagScan::missing.
enum {
    MarkdownTagEnd_Comment,         // "-->"
    MarkdownTagEnd_Instruction,     // "?>"
    MarkdownTagEnd_Cdata,           // "]]>"
    MarkdownTagEnd_Declaration,     // ">"
    MarkdownTagEnd_DoubleQuote,
    MarkdownTagEnd_SingleQuote,
};
typedef uint8_t MarkdownTagEnd;

constexpr size_t kNotMissing = ~size_t(0);

// Returns the index after the first `needle` at or after `start`, or 0 if there is none. A scan that already failed to
// find the needle after an earlier offset does not search.
size_t after(PrStringView text, size_t start, PrStringView needle, MarkdownTagScan* PR_NULLABLE scan,
             MarkdownTagEnd end) {
    if (start > text.size || (scan != nullptr && start >= scan->missing[end]))
        return 0;
    PrStringView rest = stringSlice(text, start, text.size - start);
    size_t found = stringFind(rest, needle);
    if (found == rest.size) {
        if (scan != nullptr && start < scan->missing[end])
            scan->missing[end] = start;
        return 0;
    }
    return start + found + needle.size;
}

// Returns the index after the attributes, optional whitespace, optional '/' and '>' of an open tag whose name ends at
// `start`, or 0 if the tag does not end there (commonmark/#open-tag).
size_t openTagRest(PrStringView text, size_t start, MarkdownTagScan* PR_NULLABLE scan) {
    size_t end = start;
    while (true) {
        size_t space = skipWhitespace(text, end);
        char c = at(text, space);
        if (c == '>')
            return space + 1;
        if (c == '/')
            return at(text, space + 1) == '>' ? space + 2 : 0;
        // An attribute needs whitespace before it.
        if (space == end || !(asciiIsLetter(c) || c == '_' || c == ':'))
            return 0;
        size_t name = space + 1;
        while (asciiIsLetter(at(text, name)) || asciiIsDigit(at(text, name)) || at(text, name) == '_'
               || at(text, name) == '.' || at(text, name) == ':' || at(text, name) == '-')
            ++name;
        end = name;
        size_t equals = skipWhitespace(text, name);
        if (at(text, equals) != '=')
            continue;
        size_t value = skipWhitespace(text, equals + 1);
        char quote = at(text, value);
        if (quote == '"' || quote == '\'') {
            size_t close = after(text, value + 1, PrStringView{&quote, 1}, scan,
                                 quote == '"' ? MarkdownTagEnd_DoubleQuote : MarkdownTagEnd_SingleQuote);
            if (close == 0)
                return 0;
            end = close;
        } else {
            size_t close = value;
            while (close < text.size && !markdownIsSpaceOrTab(text.data[close]) && text.data[close] != '\n'
                   && text.data[close] != '\r' && text.data[close] != '"' && text.data[close] != '\''
                   && text.data[close] != '=' && text.data[close] != '<' && text.data[close] != '>'
                   && text.data[close] != '`')
                ++close;
            if (close == value)
                return 0;
            end = close;
        }
    }
}

// Returns the length of the open tag or the closing tag at `start`, or 0 if neither starts there.
size_t openOrClosingTag(PrStringView text, size_t start, PrStringView* outName, MarkdownTagScan* PR_NULLABLE scan) {
    if (at(text, start) != '<')
        return 0;
    if (at(text, start + 1) == '/') {
        size_t name = tagName(text, start + 2);
        if (name == 0)
            return 0;
        size_t end = skipWhitespace(text, start + 2 + name);
        if (at(text, end) != '>')
            return 0;
        *outName = stringSlice(text, start + 2, name);
        return end + 1 - start;
    }
    size_t name = tagName(text, start + 1);
    if (name == 0)
        return 0;
    size_t end = openTagRest(text, start + 1 + name, scan);
    if (end == 0)
        return 0;
    *outName = stringSlice(text, start + 1, name);
    return end - start;
}

// Returns true if the text at `start` in `text` begins with `prefix`, compared without regard to ASCII case.
bool startsWithFold(PrStringView text, size_t start, PrStringView prefix) {
    return start <= text.size && asciiStartsWithFold(stringSlice(text, start, text.size - start), prefix);
}

bool containsFold(PrStringView text, PrStringView needle) {
    return asciiContainsFold(text, needle);
}

void appendCodePoint(Array<char>* out, uint32_t codePoint) {
    char bytes[kPrUtf8MaxBytes];
    out->append(bytes, prUtf8Encode(codePoint, bytes));
}

} // namespace

bool markdownIsAsciiPunctuation(char c) {
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

bool markdownIsPunctuation(uint32_t codePoint) {
    if (codePoint < 0x80)
        return markdownIsAsciiPunctuation(char(codePoint));
    switch (prUnicodeGeneralCategory(codePoint)) {
        case PrUnicodeGeneralCategory_ConnectorPunctuation:
        case PrUnicodeGeneralCategory_DashPunctuation:
        case PrUnicodeGeneralCategory_OpenPunctuation:
        case PrUnicodeGeneralCategory_ClosePunctuation:
        case PrUnicodeGeneralCategory_InitialPunctuation:
        case PrUnicodeGeneralCategory_FinalPunctuation:
        case PrUnicodeGeneralCategory_OtherPunctuation:
        case PrUnicodeGeneralCategory_MathSymbol:
        case PrUnicodeGeneralCategory_CurrencySymbol:
        case PrUnicodeGeneralCategory_ModifierSymbol:
        case PrUnicodeGeneralCategory_OtherSymbol:
            return true;
        default:
            return false;
    }
}

bool markdownIsWhitespace(uint32_t codePoint) {
    if (codePoint < 0x80)
        return codePoint == ' ' || codePoint == '\t' || codePoint == '\n' || codePoint == '\f' || codePoint == '\r';
    return prUnicodeGeneralCategory(codePoint) == PrUnicodeGeneralCategory_SpaceSeparator;
}

MarkdownTagScan markdownTagScanStart() {
    MarkdownTagScan scan;
    for (size_t& missing : scan.missing)
        missing = kNotMissing;
    return scan;
}

size_t markdownScanHtmlTag(PrStringView text, size_t start, MarkdownTagScan* scan) {
    if (at(text, start) != '<')
        return 0;
    PrStringView name{};
    size_t tag = openOrClosingTag(text, start, &name, scan);
    if (tag != 0)
        return tag;
    char next = at(text, start + 1);
    if (next == '?') {
        size_t end = after(text, start + 2, "?>", scan, MarkdownTagEnd_Instruction);
        return end == 0 ? 0 : end - start;
    }
    if (next != '!')
        return 0;
    if (startsWithFold(text, start, "<!--")) {
        if (startsWithFold(text, start, "<!-->"))
            return 5;
        if (startsWithFold(text, start, "<!--->"))
            return 6;
        size_t end = after(text, start + 4, "-->", scan, MarkdownTagEnd_Comment);
        return end == 0 ? 0 : end - start;
    }
    if (stringStartsWith(stringSlice(text, start, text.size - start), "<![CDATA[")) {
        size_t end = after(text, start + 9, "]]>", scan, MarkdownTagEnd_Cdata);
        return end == 0 ? 0 : end - start;
    }
    if (asciiIsLetter(at(text, start + 2))) {
        size_t end = after(text, start + 3, ">", scan, MarkdownTagEnd_Declaration);
        return end == 0 ? 0 : end - start;
    }
    return 0;
}

uint8_t markdownHtmlBlockStart(PrStringView line, size_t start, bool interruptsParagraph) {
    if (at(line, start) != '<')
        return 0;
    size_t name = tagName(line, start + 1);
    if (name != 0) {
        PrStringView tag = stringSlice(line, start + 1, name);
        char next = at(line, start + 1 + name);
        bool ends = isLineEnd(line, start + 1 + name);
        if (isOneOf(tag, kRawTagNames, countOf(kRawTagNames)) && (ends || markdownIsSpaceOrTab(next) || next == '>'))
            return 1;
    }
    if (startsWithFold(line, start, "<!--"))
        return 2;
    if (at(line, start + 1) == '?')
        return 3;
    if (at(line, start + 1) == '!' && asciiIsLetter(at(line, start + 2)))
        return 4;
    if (stringStartsWith(stringSlice(line, start, line.size - start), "<![CDATA["))
        return 5;
    size_t nameStart = at(line, start + 1) == '/' ? start + 2 : start + 1;
    size_t blockName = tagName(line, nameStart);
    if (blockName != 0 && isOneOf(stringSlice(line, nameStart, blockName), kBlockTagNames, countOf(kBlockTagNames))) {
        size_t end = nameStart + blockName;
        char next = at(line, end);
        if (isLineEnd(line, end) || markdownIsSpaceOrTab(next) || next == '>'
            || (next == '/' && at(line, end + 1) == '>'))
            return 6;
    }
    if (interruptsParagraph)
        return 0;
    PrStringView tag{};
    size_t length = openOrClosingTag(line, start, &tag, nullptr);
    if (length == 0 || (at(line, start + 1) != '/' && isOneOf(tag, kRawTagNames, countOf(kRawTagNames))))
        return 0;
    // The tag is complete only if it is on this line, followed by nothing but spaces and tabs.
    PrStringView taken = stringSlice(line, start, length);
    if (stringFind(taken, '\n') != taken.size || stringFind(taken, '\r') != taken.size)
        return 0;
    size_t end = start + length;
    while (markdownIsSpaceOrTab(at(line, end)))
        ++end;
    return isLineEnd(line, end) ? 7 : 0;
}

bool markdownHtmlBlockEnds(uint8_t kind, PrStringView line) {
    switch (kind) {
        case 1:
            return containsFold(line, "</pre>") || containsFold(line, "</script>") || containsFold(line, "</style>")
                || containsFold(line, "</textarea>");
        case 2:
            return stringContains(line, "-->");
        case 3:
            return stringContains(line, "?>");
        case 4:
            return stringContains(line, ">");
        case 5:
            return stringContains(line, "]]>");
        default:
            return false;
    }
}

size_t markdownScanUriAutolink(PrStringView text, size_t start) {
    if (at(text, start) != '<' || !asciiIsLetter(at(text, start + 1)))
        return 0;
    size_t end = start + 2;
    while (asciiIsLetter(at(text, end)) || asciiIsDigit(at(text, end)) || at(text, end) == '+' || at(text, end) == '.'
           || at(text, end) == '-')
        ++end;
    size_t scheme = end - (start + 1);
    if (scheme < 2 || scheme > 32 || at(text, end) != ':')
        return 0;
    ++end;
    while (end < text.size && text.data[end] != '>') {
        char c = text.data[end];
        if (isAsciiControl(c) || c == ' ' || c == '<')
            return 0;
        ++end;
    }
    return end < text.size ? end + 1 - start : 0;
}

size_t markdownScanEmailAutolink(PrStringView text, size_t start) {
    if (at(text, start) != '<')
        return 0;
    size_t end = start + 1;
    while (end < text.size) {
        char c = text.data[end];
        if (!(asciiIsLetter(c) || asciiIsDigit(c) || (c != '<' && c != '>' && c != '@' && c != '"' && c != '('
                                                         && c != ')' && c != ',' && c != ':' && c != ';' && c != '['
                                                         && c != ']' && c != '\\' && markdownIsAsciiPunctuation(c))))
            break;
        ++end;
    }
    if (end == start + 1 || at(text, end) != '@')
        return 0;
    ++end;
    while (true) {
        size_t label = end;
        while (asciiIsLetter(at(text, end)) || asciiIsDigit(at(text, end)) || at(text, end) == '-')
            ++end;
        size_t size = end - label;
        if (size == 0 || size > 63 || text.data[label] == '-' || text.data[end - 1] == '-')
            return 0;
        if (at(text, end) == '>')
            return end + 1 - start;
        if (at(text, end) != '.')
            return 0;
        ++end;
    }
}

bool markdownScanLinkDestination(PrStringView text, size_t start, size_t* outLength, MarkdownSpan* out) {
    if (at(text, start) == '<') {
        size_t end = start + 1;
        while (end < text.size) {
            char c = text.data[end];
            if (c == '\\' && markdownIsAsciiPunctuation(at(text, end + 1))) {
                end += 2;
                continue;
            }
            if (c == '>') {
                if (end + 1 >= text.size)
                    return false;
                *out = MarkdownSpan{start + 1, end - start - 1};
                *outLength = end + 1 - start;
                return true;
            }
            if (c == '<' || c == '\n' || c == '\r')
                return false;
            ++end;
        }
        return false;
    }
    size_t end = start;
    uint32_t depth = 0;
    while (end < text.size) {
        char c = text.data[end];
        if (c == '\\' && markdownIsAsciiPunctuation(at(text, end + 1))) {
            end += 2;
            continue;
        }
        if (c == '(') {
            if (++depth > kMaxDestinationParentheses)
                return false;
        } else if (c == ')') {
            if (depth == 0)
                break;
            --depth;
        } else if (c == ' ' || isAsciiControl(c)) {
            // A destination is not empty if whitespace follows it.
            if (end == start && (c == ' ' || asciiIsSpace(c)))
                return false;
            break;
        }
        ++end;
    }
    // A destination ends before the text does, at the whitespace or the ')' that follows it.
    if (depth != 0 || end >= text.size)
        return false;
    *out = MarkdownSpan{start, end - start};
    *outLength = end - start;
    return true;
}

size_t markdownScanLinkTitle(PrStringView text, size_t start, MarkdownSpan* out) {
    char open = at(text, start);
    char close = open == '(' ? ')' : open;
    if (open != '"' && open != '\'' && open != '(')
        return 0;
    size_t end = start + 1;
    while (end < text.size) {
        char c = text.data[end];
        if (c == '\\' && markdownIsAsciiPunctuation(at(text, end + 1))) {
            end += 2;
            continue;
        }
        if (c == close) {
            *out = MarkdownSpan{start + 1, end - start - 1};
            return end + 1 - start;
        }
        if (open == '(' && c == '(')
            return 0;
        ++end;
    }
    return 0;
}

void markdownAppendFilteredHtml(PrStringView html, Array<char>* out) {
    static const char* const kDisallowed[] = {
        "title", "textarea", "style", "xmp", "iframe", "noembed", "noframes", "script", "plaintext",
    };
    size_t run = 0;
    for (size_t index = 0; index < html.size; ++index) {
        if (html.data[index] != '<')
            continue;
        size_t name = at(html, index + 1) == '/' ? index + 2 : index + 1;
        size_t length = tagName(html, name);
        if (length == 0 || !isOneOf(stringSlice(html, name, length), kDisallowed, countOf(kDisallowed)))
            continue;
        char next = at(html, name + length);
        if (!(asciiIsSpace(next) || next == '>' || (next == '/' && at(html, name + length + 1) == '>')))
            continue;
        out->append(html.data + run, uint32_t(index - run));
        out->append("&lt;", 4);
        run = index + 1;
    }
    out->append(html.data + run, uint32_t(html.size - run));
}

bool markdownIsUnsafeDestination(PrStringView destination) {
    if (asciiStartsWithFold(destination, "data:image/")) {
        PrStringView type = stringSlice(destination, 11, destination.size - 11);
        if (asciiStartsWithFold(type, "png") || asciiStartsWithFold(type, "gif") || asciiStartsWithFold(type, "jpeg")
            || asciiStartsWithFold(type, "webp"))
            return false;
    }
    return asciiStartsWithFold(destination, "javascript:") || asciiStartsWithFold(destination, "vbscript:")
        || asciiStartsWithFold(destination, "file:") || asciiStartsWithFold(destination, "data:");
}

size_t markdownSkipSpaceAndLineEnding(PrStringView text, size_t start) {
    size_t end = start;
    while (markdownIsSpaceOrTab(at(text, end)))
        ++end;
    if (at(text, end) == '\r')
        ++end;
    if (at(text, end) == '\n')
        ++end;
    while (markdownIsSpaceOrTab(at(text, end)))
        ++end;
    return end;
}

size_t markdownDecodeReference(PrStringView text, size_t start, Array<char>* out) {
    if (at(text, start) != '&')
        return 0;
    if (at(text, start + 1) == '#') {
        bool hex = at(text, start + 2) == 'x' || at(text, start + 2) == 'X';
        size_t digits = start + (hex ? 3 : 2);
        size_t end = digits;
        uint32_t value = 0;
        while (end - digits < (hex ? kMaxHexDigits : kMaxDecimalDigits)
               && (hex ? asciiIsHexDigit(at(text, end)) : asciiIsDigit(at(text, end)))) {
            value = value * (hex ? 16 : 10) + asciiHexValue(text.data[end]);
            ++end;
        }
        if (end == digits || at(text, end) != ';')
            return 0;
        bool valid = value != 0 && value <= kPrUtf8MaxCodePoint && !(value >= 0xd800 && value <= 0xdfff);
        appendCodePoint(out, valid ? value : kPrUtf8Replacement);
        return end + 1 - start;
    }
    size_t end = start + 1;
    while (end - start - 1 < kPrHtmlEntityMaxLength && (asciiIsLetter(at(text, end)) || asciiIsDigit(at(text, end))))
        ++end;
    if (end == start + 1 || at(text, end) != ';')
        return 0;
    PrStringView expansion = prHtmlEntityMatch(stringSlice(text, start + 1, end + 1 - (start + 1)));
    if (expansion.data == nullptr)
        return 0;
    out->append(expansion.data, uint32_t(expansion.size));
    return end + 1 - start;
}

void markdownAppendDecoded(PrStringView text, Array<char>* out) {
    size_t run = 0;
    size_t index = 0;
    while (index < text.size) {
        char c = text.data[index];
        if (c == '\\' && markdownIsAsciiPunctuation(at(text, index + 1))) {
            out->append(text.data + run, uint32_t(index - run));
            out->push(text.data[index + 1]);
            index += 2;
            run = index;
            continue;
        }
        if (c == '&') {
            uint32_t mark = out->count;
            out->append(text.data + run, uint32_t(index - run));
            size_t length = markdownDecodeReference(text, index, out);
            if (length != 0) {
                index += length;
                run = index;
                continue;
            }
            out->resize(mark);
        }
        ++index;
    }
    out->append(text.data + run, uint32_t(index - run));
}

bool markdownAppendNormalizedLabel(PrStringView label, Array<char>* out) {
    uint32_t mark = out->count;
    bool space = false;
    size_t index = 0;
    while (index < label.size) {
        uint32_t codePoint = 0;
        index += prUtf8Decode(label, index, &codePoint);
        if (markdownIsWhitespace(codePoint)) {
            space = out->count != mark;
            continue;
        }
        if (space)
            out->push(' ');
        space = false;
        uint32_t folded[3];
        uint32_t count = prUnicodeFold(codePoint, folded);
        for (uint32_t i = 0; i < count; ++i)
            appendCodePoint(out, folded[i]);
    }
    return out->count != mark;
}

} // namespace pr_impl
