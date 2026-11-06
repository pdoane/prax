// Links in a terminal's text, and the text of a line to search for them.

#include "studio/terminal/src/terminal_link.h"

#include "pr/string_ops.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// The most digits in a line or column number, which keeps the number within 32 bits.
constexpr uint32_t kMaxDigits = 9;

// The prefixes that start a URL.
constexpr PrStringView kUrlPrefixes[] = {"http://", "https://"};

// Returns true if `c` can be part of a path as tools print one. That is anything except white space, quotes, brackets,
// and the punctuation that separates a path from what follows it.
bool isPathByte(char c) {
    switch (c) {
        case ' ':
        case '\t':
        case '"':
        case '\'':
        case '`':
        case '(':
        case ')':
        case '<':
        case '>':
        case '[':
        case ']':
        case '{':
        case '}':
        case ',':
        case ';':
        case '|':
        case ':':
            return false;
        default:
            return c != 0;
    }
}

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

bool isLetter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isSpace(char c) {
    return c == ' ' || c == '\t';
}

// isUrlLead returns true if `c` can precede a URL, and isUrlTrail returns true if `c` can follow one. Neither character
// is part of the URL.
bool isUrlLead(char c) {
    return c == '(' || c == '<' || c == '[' || c == '{' || c == '\'' || c == '"';
}

bool isUrlTrail(char c) {
    switch (c) {
        case ')':
        case '>':
        case ']':
        case '}':
        case '\'':
        case '"':
        case '.':
        case ',':
        case ';':
        case ':':
        case '!':
        case '?':
            return true;
        default:
            return false;
    }
}

// Reads a number of 1 to kMaxDigits digits at `*at`, and moves `*at` past it. Returns false, and leaves `*at`
// unchanged, if no such number is there.
bool readNumber(PrStringView text, uint32_t* at, uint32_t* number) {
    uint32_t i = *at;
    uint32_t value = 0;
    while (i < text.size && isDigit(text.data[i]) && i - *at < kMaxDigits)
        value = value * 10 + uint32_t(text.data[i++] - '0');
    if (i == *at || (i < text.size && isDigit(text.data[i])))
        return false;
    *at = i;
    *number = value;
    return true;
}

// Returns true if `text` has `prefix` at offset `at`.
bool hasAt(PrStringView text, uint32_t at, PrStringView prefix) {
    return at <= text.size && prStringStartsWith(PrStringView{text.data + at, text.size - at}, prefix);
}

// Reads a URL from the word that begins at `at`, and writes the end of the word to `*wordEnd`. Returns false if the
// word holds no URL.
bool readUrl(PrStringView text, uint32_t at, TerminalTextLink* link, uint32_t* wordEnd) {
    uint32_t end = at;
    while (end < text.size && !isSpace(text.data[end]))
        ++end;
    *wordEnd = end;
    uint32_t start = at;
    while (start < end && isUrlLead(text.data[start]))
        ++start;
    while (end > start && isUrlTrail(text.data[end - 1]))
        --end;
    PrStringView url = prStringSlice(text, start, end - start);
    for (PrStringView prefix : kUrlPrefixes) {
        if (url.size > prefix.size && prStringStartsWith(url, prefix)) {
            *link = TerminalTextLink{
                .kind = TerminalTextLinkKind_Url,
                .start = start,
                .end = end,
                .targetStart = start,
                .targetEnd = end,
            };
            return true;
        }
    }
    return false;
}

// Reads the line and column after a path that ends at `at`, in the form `:line`, `:line:column`, `(line)` or
// `(line,column)`. Writes the end of what it read to `*end`. Returns false if no line follows the path.
bool readPosition(PrStringView text, uint32_t at, TerminalTextLink* link, uint32_t* end) {
    uint32_t i = at;
    if (i < text.size && text.data[i] == ':') {
        ++i;
        if (!readNumber(text, &i, &link->line))
            return false;
        uint32_t column = i + 1;
        if (i < text.size && text.data[i] == ':' && readNumber(text, &column, &link->column))
            i = column;
        *end = i;
        return true;
    }
    if (i < text.size && text.data[i] == '(') {
        ++i;
        if (!readNumber(text, &i, &link->line))
            return false;
        if (i < text.size && text.data[i] == ',') {
            ++i;
            if (!readNumber(text, &i, &link->column))
                return false;
        }
        if (i >= text.size || text.data[i] != ')')
            return false;
        *end = i + 1;
        return true;
    }
    return false;
}

// Reads a Python traceback line, `File "path", line N`, at `at`. Returns false if none begins there.
bool readTraceback(PrStringView text, uint32_t at, TerminalTextLink* link) {
    constexpr PrStringView kFile = "File \"";
    constexpr PrStringView kLine = "\", line ";
    if (!hasAt(text, at, kFile))
        return false;
    uint32_t path = at + uint32_t(kFile.size);
    uint32_t quote = path;
    while (quote < text.size && text.data[quote] != '"')
        ++quote;
    uint32_t i = quote + uint32_t(kLine.size);
    if (quote == path || !hasAt(text, quote, kLine) || !readNumber(text, &i, &link->line))
        return false;
    link->start = path;
    link->end = i;
    link->targetStart = path;
    link->targetEnd = quote;
    link->column = 0;
    return true;
}

// Returns true if the text from `start` to `end` looks like a path even with no line after it. That is the case if it
// has a separator, or a dot followed by a character other than a dot.
bool looksLikePath(PrStringView text, uint32_t start, uint32_t end) {
    for (uint32_t i = start; i < end; ++i) {
        char c = text.data[i];
        if (c == '/' || c == '\\' || (c == '.' && i + 1 < end && text.data[i + 1] != '.'))
            return true;
    }
    return false;
}

} // namespace

void terminalTextLinks(PrStringView text, RawArray<TerminalTextLink>* out) {
    uint32_t size = uint32_t(text.size);
    uint32_t i = 0;
    while (i < size) {
        TerminalTextLink link{.kind = TerminalTextLinkKind_File};
        uint32_t wordEnd = i;
        if ((i == 0 || isSpace(text.data[i - 1])) && readUrl(text, i, &link, &wordEnd)) {
            // The rest of the word around the URL is not a path.
            out->push(link);
            i = wordEnd;
            continue;
        }
        if (readTraceback(text, i, &link)) {
            out->push(link);
            i = link.end;
            continue;
        }
        if (!isPathByte(text.data[i])) {
            ++i;
            continue;
        }
        uint32_t start = i;
        uint32_t end = i;
        while (end < size && isPathByte(text.data[end]))
            ++end;
        // The colon of a Windows drive, as in C:\, is part of the path.
        bool drive = end == start + 1 && isLetter(text.data[start]) && end + 1 < size && text.data[end] == ':' &&
            (text.data[end + 1] == '\\' || text.data[end + 1] == '/');
        if (drive) {
            end += 2;
            while (end < size && isPathByte(text.data[end]))
                ++end;
        }
        uint32_t after = end;
        if (readPosition(text, end, &link, &after)) {
            link.start = start;
            link.end = after;
            link.targetStart = start;
            link.targetEnd = end;
            out->push(link);
            i = after;
            continue;
        }
        // A period at the end of a sentence is not part of the path.
        uint32_t last = end;
        while (last > start && text.data[last - 1] == '.')
            --last;
        if (looksLikePath(text, start, last)) {
            out->push(TerminalTextLink{
                .kind = TerminalTextLinkKind_File,
                .start = start,
                .end = last,
                .targetStart = start,
                .targetEnd = last,
            });
        }
        i = end;
    }
}

void terminalLineText(const PrVterm* vterm, uint32_t index, TerminalLineText* out) {
    PrVtermLine line = prVtermLine(vterm, index);
    for (uint32_t column = 0; column < line.cellCount; ++column) {
        const PrVtermCell& cell = line.cells[column];
        if ((cell.flags & (PrVtermCellFlag_WideTail | PrVtermCellFlag_WideSpacer)) != 0)
            continue;
        for (uint32_t i = 0; i <= cell.clusterCount; ++i) {
            uint32_t codePoint = i == 0 ? cell.codePoint : line.codePoints[cell.clusterStart + i - 1];
            char bytes[kPrUtf8MaxBytes];
            uint32_t size = prUtf8Encode(codePoint != 0 ? codePoint : ' ', bytes);
            out->text.append(bytes, size);
            for (uint32_t b = 0; b < size; ++b)
                out->columns.push(column);
        }
    }
    for (uint32_t column = line.cellCount; column < prVtermExtent(vterm).x; ++column) {
        out->text.push(' ');
        out->columns.push(column);
    }
}

void terminalLineTextReset(TerminalLineText* text) {
    text->text.reset();
    text->columns.reset();
}

} // namespace pr_impl
