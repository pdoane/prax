#include "css/src/css_syntax.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/bit_cast.h"
#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/string_ops.h"
#include "pr/string_trim.h"
#include "pr/utf8.h"

// The tokenizer reads the source in place, one token at a time. The class of a token's first byte selects the algorithm
// of css-syntax-3/#tokenizer-algorithms that consumes it. The algorithms read bytes through class tables.

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Decoding (css-syntax-3/#input-byte-stream)

PrStringView cssDecodeBytes(PrStringView bytes, PrCharset protocol, PrCharset environment, PrString* out) {
    PrCharset charset = prCharsetSniffBom(bytes);
    if (charset == PrCharset_None)
        charset = protocol;
    // css-syntax-3/#determine-the-fallback-encoding: an @charset rule is read from the sheet's first 1024 bytes, and
    // only in the form that the grammar writes it, with its label between `@charset "` and `";`.
    constexpr PrStringView kOpening = "@charset \"";
    if (charset == PrCharset_None && prStringStartsWith(bytes, kOpening)) {
        size_t prefix = bytes.size < 1024 ? bytes.size : 1024;
        PrStringView start = prStringSlice(bytes, kOpening.size, prefix - kOpening.size);
        size_t end = prStringFind(start, "\";");
        if (end != start.size) {
            charset = prCharsetFromLabel(prStringSlice(start, 0, end));
            if (charset == PrCharset_Utf16Be || charset == PrCharset_Utf16Le)
                charset = PrCharset_Utf8;
        }
    }
    if (charset == PrCharset_None)
        charset = environment != PrCharset_None ? environment : PrCharset_Utf8;
    PrCharsetDecoder decoder = prCharsetDecoderMake(charset, 0);
    return prCharsetDecodeView(&decoder, bytes, PrCharsetInput_Final, out);
}

// ---------------------------------------------------------------------------------------------------------------------
// Byte classes (css-syntax-3/#tokenizer-definitions)

constexpr CssByteTable makeCssBytes() {
    CssByteTable table = {};
    for (uint32_t b = 0; b < 256; ++b) {
        bool letter = prAsciiIsLetter(char(b));
        bool space = prAsciiIsSpace(char(b));
        bool control = b < 0x20 || b == 0x7f;

        CssByteFlags bits = 0;
        if (letter || prAsciiIsDigit(char(b)) || b == '-' || b == '_')
            bits |= CssByteFlag_Ident;
        if (letter || b == '_')
            bits |= CssByteFlag_Start;
        if (space)
            bits |= CssByteFlag_Space;
        if (b == '\n' || b == '\r' || b == '\f')
            bits |= CssByteFlag_Newline;
        if (b >= 0x80 || b == 0)
            bits |= CssByteFlag_Decode;
        if (b == 0 || b == '\r' || b == '\f')
            bits |= CssByteFlag_Rewritten;
        if (control)
            bits |= CssByteFlag_Control;

        // A null is printable after preprocessing, which turns it into U+FFFD.
        if (b == '"' || b == '\'' || b == '(' || (control && !space && b != 0))
            bits |= CssByteFlag_UrlEnd;
        table.of[b] = bits;
    }
    return table;
}

constexpr CssByteTable kCssByte = makeCssBytes();

struct Punctuation {
    char byte;
    CssTokenKind kind;
};

constexpr Punctuation kPunctuation[] = {
    {':', CssTokenKind_Colon},
    {';', CssTokenKind_Semicolon},
    {',', CssTokenKind_Comma},
    {'[', CssTokenKind_OpenBracket},
    {']', CssTokenKind_CloseBracket},
    {'(', CssTokenKind_OpenParen},
    {')', CssTokenKind_CloseParen},
    {'{', CssTokenKind_OpenBrace},
    {'}', CssTokenKind_CloseBrace},
};

constexpr CssPunctuationTable makePunctuation() {
    CssPunctuationTable table = {};
    for (uint32_t b = 0; b < 256; ++b)
        table.kindOf[b] = CssTokenKind_Delim;
    for (Punctuation punctuation : kPunctuation) {
        table.kindOf[uint8_t(punctuation.byte)] = punctuation.kind;
        table.byteOf[punctuation.kind] = punctuation.byte;
    }
    return table;
}

constexpr CssPunctuationTable kCssPunctuation = makePunctuation();

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// The source

// The value that `at` returns past the end of the source. No byte has this value.
constexpr uint32_t kEnd = 0x100;

struct Source {
    const char* text;
    uint32_t size;
};

// Returns the byte at `pos`, or kEnd past the end.
uint32_t at(const Source* s, uint32_t pos) {
    return pos < s->size ? uint8_t(s->text[pos]) : kEnd;
}

// Writes the code point at `pos` to `codePoint`, and returns its width.
uint32_t decodeAt(const Source* s, uint32_t pos, uint32_t* codePoint) {
    return prUtf8DecodeScalar(PrStringView{s->text, s->size}, pos, codePoint);
}

// ---------------------------------------------------------------------------------------------------------------------
// Byte classes of the token loop

// The class of a byte as the first byte of a token (css-syntax-3/#consume-token). kEnd has an entry of its own.
enum {
    Lead_Single,       // a token of that byte on its own: punctuation or a delim
    Lead_Space,
    Lead_Name,         // an ident-start byte
    Lead_Digit,
    Lead_Quote,
    Lead_Hash,
    Lead_Plus,
    Lead_Hyphen,
    Lead_Dot,
    Lead_Less,
    Lead_At,
    Lead_Slash,
    Lead_Backslash,
    Lead_Wide,         // a null or a byte above ASCII, which must be decoded
    Lead_End,          // kEnd
};
typedef uint8_t Lead;

struct LeadByte {
    char byte;
    Lead lead;
};

constexpr LeadByte kLeadBytes[] = {
    {'"', Lead_Quote},
    {'\'', Lead_Quote},
    {'#', Lead_Hash},
    {'+', Lead_Plus},
    {'-', Lead_Hyphen},
    {'.', Lead_Dot},
    {'<', Lead_Less},
    {'@', Lead_At},
    {'/', Lead_Slash},
    {'\\', Lead_Backslash},
};

struct LeadTable {
    Lead of[kEnd + 1];
};

constexpr LeadTable makeLeads() {
    LeadTable table = {};
    for (uint32_t b = 0; b < 256; ++b) {
        CssByteFlags classes = kCssByte.of[b];
        Lead lead = Lead_Single;
        if ((classes & CssByteFlag_Space) != 0)
            lead = Lead_Space;
        else if ((classes & CssByteFlag_Start) != 0)
            lead = Lead_Name;
        else if ((classes & CssByteFlag_Decode) != 0)
            lead = Lead_Wide;
        else if (prAsciiIsDigit(char(b)))
            lead = Lead_Digit;
        table.of[b] = lead;
    }
    for (LeadByte entry : kLeadBytes)
        table.of[uint8_t(entry.byte)] = entry.lead;
    table.of[kEnd] = Lead_End;
    return table;
}

constexpr LeadTable kLead = makeLeads();

// The byte classes that the loops inside a token read. kEnd is in none of them.
enum {
    // Ends the content of a string opened by '"': that quote, a backslash or a newline.
    ScanFlag_Double = 1 << 0,

    ScanFlag_Single = 1 << 1,    // the same for a string opened by '\''
    ScanFlag_Control = 1 << 2,   // the bytes of CssByteFlag_Control
    ScanFlag_Decode = 1 << 3,    // the bytes of CssByteFlag_Decode

    // A backslash, a null or a byte above ASCII. Each of these may continue an ident sequence past its ASCII name
    // bytes.
    ScanFlag_More = 1 << 4,
};
typedef uint8_t ScanFlags;

struct ScanTable {
    ScanFlags of[kEnd + 1];
};

constexpr ScanTable makeScans() {
    ScanTable table = {};
    for (uint32_t b = 0; b < 256; ++b) {
        CssByteFlags classes = kCssByte.of[b];
        ScanFlags bits = 0;
        if ((classes & CssByteFlag_Newline) != 0 || b == '\\')
            bits |= ScanFlag_Double | ScanFlag_Single;
        if (b == '"')
            bits |= ScanFlag_Double;
        if (b == '\'')
            bits |= ScanFlag_Single;
        if ((classes & CssByteFlag_Control) != 0)
            bits |= ScanFlag_Control;
        if ((classes & CssByteFlag_Decode) != 0)
            bits |= ScanFlag_Decode | ScanFlag_More;
        if (b == '\\')
            bits |= ScanFlag_More;
        table.of[b] = bits;
    }
    return table;
}

constexpr ScanTable kScan = makeScans();

// ---------------------------------------------------------------------------------------------------------------------
// Consuming a token (css-syntax-3/#tokenizer-algorithms)
//
// Each algorithm of the spec is one function. A function receives the bytes that its caller already read.

// A consumed token: its end, its kind, its flags other than Contiguous, and a dimension's unit. A consumer sets
// PlainText from the bytes that it read. It sets Verbatim from what serializing the token's kind writes
// (css-syntax-3/#serialization).
struct Consumed {
    uint32_t end;
    CssTokenKind kind;
    CssTokenFlags flags;
    PrCssUnit unit;
};

Consumed consumed(uint32_t end, CssTokenKind kind, CssTokenFlags flags) {
    return Consumed{end, kind, flags, PrCssUnit_Number};
}

// Returns a delim token for the ASCII byte at `pos`, which serializes as that byte.
Consumed delimAt(uint32_t pos) {
    return consumed(pos + 1, CssTokenKind_Delim, CssTokenFlag_PlainVerbatim);
}

// Returns `flags` with Verbatim added if PlainText is set, for a token that serializes as its bytes whenever they are
// its value.
CssTokenFlags verbatimWhenPlain(CssTokenFlags flags) {
    if ((flags & CssTokenFlag_PlainText) != 0)
        flags |= CssTokenFlag_Verbatim;
    return flags;
}

// Returns the end of the whitespace that starts at `pos`.
uint32_t skipSpace(const Source* s, uint32_t pos) {
    while (pos < s->size && (kCssByte.of[uint8_t(s->text[pos])] & CssByteFlag_Space) != 0)
        ++pos;
    return pos;
}

// Returns true if the backslash at `pos` starts a valid escape (css-syntax-3/#starts-with-a-valid-escape). A backslash
// at the end of the input also starts an escape, which css-syntax-3/#consume-escaped-code-point turns into U+FFFD.
bool validEscape(const Source* s, uint32_t pos) {
    uint32_t after = at(s, pos + 1);
    return after == kEnd || (kCssByte.of[after] & CssByteFlag_Newline) == 0;
}

// Consumes an escaped code point (css-syntax-3/#consume-escaped-code-point). Returns the position one past the escape
// whose backslash is at `pos`. The escape's value is read later.
uint32_t consumeEscape(const Source* s, uint32_t pos) {
    uint32_t byte = at(s, ++pos);
    if (byte == kEnd)
        return pos;
    if (!prAsciiIsHexDigit(char(byte)))
        return pos + ((kCssByte.of[byte] & CssByteFlag_Decode) != 0 ? decodeAt(s, pos, &byte) : 1);

    for (uint32_t digits = 0; digits < 6 && prAsciiIsHexDigit(char(byte)); ++digits)
        byte = at(s, ++pos);

    // One whitespace code point ends the escape, and a CRLF counts as one.
    if (byte != kEnd && (kCssByte.of[byte] & CssByteFlag_Space) != 0)
        pos += byte == '\r' && at(s, pos + 1) == '\n' ? 2u : 1u;
    return pos;
}

// Returns true if the code point at `pos`, whose first byte is `byte`, starts an ident sequence with no '-' before it.
// That holds for an ident-start code point or a valid escape. Every code point above ASCII is an ident-start code
// point, where css-syntax-3/#non-ascii-ident-code-point allows only some of them, since content relies on the rest
// (csswg-drafts#11005).
bool identStart(const Source* s, uint32_t pos, uint32_t byte) {
    switch (kLead.of[byte]) {
        case Lead_Name:
            return true;
        case Lead_Backslash:
            return validEscape(s, pos);
        case Lead_Wide:
            return true;
        default:
            return false;
    }
}

// Returns true if the bytes at `pos`, the first of which is `byte`, would start an ident sequence
// (css-syntax-3/#would-start-an-identifier).
bool startsIdent(const Source* s, uint32_t pos, uint32_t byte) {
    if (byte != '-')
        return identStart(s, pos, byte);
    uint32_t second = at(s, pos + 1);
    return second == '-' || identStart(s, pos + 1, second);
}

// Returns true if the bytes after the sign at `pos` start a number (css-syntax-3/#starts-with-a-number). The first of
// those bytes is `second`.
bool numberAfterSign(const Source* s, uint32_t pos, uint32_t second) {
    return prAsciiIsDigit(char(second)) || (second == '.' && prAsciiIsDigit(char(at(s, pos + 2))));
}

// Returns true if the bytes at `pos` start a unicode range (css-syntax-3/#starts-a-unicode-range): `u`, `+`, then a hex
// digit or a question mark. `byte` is the byte at `pos`.
bool startsUnicodeRange(const Source* s, uint32_t pos, uint32_t byte) {
    if (prAsciiLower(char(byte)) != 'u' || at(s, pos + 1) != '+')
        return false;
    uint32_t third = at(s, pos + 2);
    return third == '?' || prAsciiIsHexDigit(char(third));
}

// The position where a name ends, and the byte at that position.
struct NameEnd {
    uint32_t pos;
    uint32_t byte;    // kEnd at the end of the source
};

// The number of source bytes that are read at once, as a uint64_t.
constexpr uint32_t kWord = sizeof(uint64_t);

// Returns a word with every byte set to `byte`.
constexpr uint64_t wordOf(uint32_t byte) {
    return 0x0101010101010101ull * byte;
}

// Returns the high bit of every byte of `word` that is an ASCII name byte. The low seven bits of a byte are at or above
// `b` if adding 0x80 - b to them carries into the high bit. They equal `b` if adding 0x7f to their xor with `b` does
// not carry. No sum carries out of its byte.
constexpr uint64_t nameBytes(uint64_t word) {
    uint64_t low = word & wordOf(0x7f);
    uint64_t folded = low | wordOf(0x20);
    uint64_t letter = (folded + wordOf(0x80 - 'a')) & ~(folded + wordOf(0x80 - 'z' - 1));
    uint64_t digit = (low + wordOf(0x80 - '0')) & ~(low + wordOf(0x80 - '9' - 1));
    uint64_t hyphen = ~((low ^ wordOf('-')) + wordOf(0x7f));
    uint64_t underscore = ~((low ^ wordOf('_')) + wordOf(0x7f));
    return (letter | digit | hyphen | underscore) & ~word & wordOf(0x80);
}

// Returns the end of the run of ASCII name bytes that starts at `pos`. After the first eight bytes, the run is read a
// word at a time.
PR_FORCE_INLINE NameEnd nameRun(const Source* s, uint32_t pos) {
    const char* text = s->text;
    uint32_t size = s->size;
    uint32_t first = size - pos > kWord ? pos + kWord : size;
    for (; pos < first; ++pos) {
        uint32_t byte = uint8_t(text[pos]);
        if ((kCssByte.of[byte] & CssByteFlag_Ident) == 0)
            return NameEnd{pos, byte};
    }
    for (; size - pos >= kWord; pos += kWord) {
        uint64_t word = loadUnaligned<uint64_t>(text + pos);
        uint64_t stops = ~nameBytes(word) & wordOf(0x80);
        if (stops != 0) {
            uint32_t shift = prU64LowestBit(stops) & ~7u;
            return NameEnd{pos + shift / 8, uint32_t(word >> shift) & 0xff};
        }
    }
    for (; pos < size; ++pos) {
        uint32_t byte = uint8_t(text[pos]);
        if ((kCssByte.of[byte] & CssByteFlag_Ident) == 0)
            return NameEnd{pos, byte};
    }
    return NameEnd{pos, kEnd};
}

// Consumes the rest of a name (css-syntax-3/#consume-name), from the backslash or the code point above ASCII at `name`,
// which ends a run of ASCII name bytes. An escape or a code point above ASCII clears PlainText from `flags`.
PR_NOINLINE NameEnd consumeNameRest(const Source* s, NameEnd name, CssTokenFlags* flags) {
    for (;;) {
        uint32_t pos = name.pos;
        if (name.byte == '\\') {
            if (!validEscape(s, pos))
                return name;
            pos = consumeEscape(s, pos);
        } else {
            uint32_t codePoint = 0;
            pos += decodeAt(s, pos, &codePoint);
        }
        *flags &= ~CssTokenFlag_PlainText;

        name = nameRun(s, pos);
        if ((kScan.of[name.byte] & ScanFlag_More) == 0)
            return name;
    }
}

// Consumes a name that starts at `pos` (css-syntax-3/#consume-name). An escape or a code point above ASCII clears
// PlainText from `flags`.
PR_FORCE_INLINE NameEnd consumeName(const Source* s, uint32_t pos, CssTokenFlags* flags) {
    NameEnd name = nameRun(s, pos);
    if ((kScan.of[name.byte] & ScanFlag_More) != 0)
        name = consumeNameRest(s, name, flags);
    return name;
}

// The parts of the number that starts a numeric token (css-syntax-3/#consume-number): the sign, the digits, the decimal
// point and its digits, and the exponent. A decimal point requires a digit after it, so the `3.` in `3.x` is the number
// `3` and a delim.
struct Number {
    uint32_t end;             // one past the number, where a unit or a '%' starts
    uint32_t after;           // the byte at `end`, or kEnd
    uint32_t integerEnd;      // one past the digits before the point
    uint32_t fractionEnd;     // one past the digits after the point, or integerEnd if there is no point
    bool sign;
    bool exponent;
};

Number readNumber(const Source* s, uint32_t pos) {
    Number n = {};
    uint32_t byte = at(s, pos);
    n.sign = byte == '+' || byte == '-';
    if (n.sign)
        byte = at(s, ++pos);
    while (prAsciiIsDigit(char(byte)))
        byte = at(s, ++pos);
    n.integerEnd = pos;

    if (byte == '.') {
        uint32_t next = at(s, pos + 1);
        if (prAsciiIsDigit(char(next))) {
            ++pos;
            byte = next;
            while (prAsciiIsDigit(char(byte)))
                byte = at(s, ++pos);
        }
    }
    n.fractionEnd = pos;

    if (prAsciiLower(char(byte)) == 'e') {
        uint32_t exponent = pos + 1;
        uint32_t next = at(s, exponent);
        if (next == '+' || next == '-')
            next = at(s, ++exponent);
        if (prAsciiIsDigit(char(next))) {
            n.exponent = true;
            pos = exponent;
            byte = next;
            while (prAsciiIsDigit(char(byte)))
                byte = at(s, ++pos);
        }
    }
    n.end = pos;
    n.after = byte;
    return n;
}

// Returns the flags that describe how the number was written. <integer> and the <an+b> shapes are defined on this form.
CssTokenFlags numberFlags(const Number* n) {
    CssTokenFlags flags = 0;
    if (n->fractionEnd == n->integerEnd && !n->exponent)
        flags |= CssTokenFlag_Integer;
    if (n->sign)
        flags |= CssTokenFlag_SignPresent;
    return flags;
}

// Returns true if the bytes of a numeric token from `start` to `end` are what cssSerializeNumber writes for it. That
// requires no sign, no exponent, no leading zero in an integer part above nine, no trailing zero in the fraction, and a
// magnitude of 0.000001 or more. It also requires at most fifteen digits, beyond which a decimal and the double that it
// names no longer agree. Finally, it requires no unit that the number would read as its exponent.
bool numberIsVerbatim(const char* text, uint32_t start, const Number* n, uint32_t end) {
    if (n->sign || n->exponent)
        return false;

    uint32_t whole = n->integerEnd - start;
    if (whole == 0 || (whole > 1 && text[start] == '0'))
        return false;

    uint32_t digits = whole;
    if (n->fractionEnd != n->integerEnd) {
        uint32_t fraction = n->integerEnd + 1;
        digits += n->fractionEnd - fraction;
        if (text[n->fractionEnd - 1] == '0')
            return false;

        // A number below 0.000001 serializes with an exponent: six zeros after the point.
        if (whole == 1 && text[start] == '0') {
            uint32_t zeros = fraction;
            while (zeros < n->fractionEnd && zeros - fraction < 6 && text[zeros] == '0')
                ++zeros;
            if (zeros - fraction == 6)
                return false;
        }
    }
    if (digits > 15)
        return false;

    // The rest is the unit. Serializing escapes its first code point if the number would read it as an exponent.
    uint32_t unit = n->end;
    if (unit == end || prAsciiLower(text[unit]) != 'e')
        return true;
    if (unit + 1 < end && prAsciiIsDigit(text[unit + 1]))
        return false;
    return !(unit + 2 < end && text[unit + 1] == '-' && prAsciiIsDigit(text[unit + 2]));
}

// Returns the unit from `start` to `end`, with escapes resolved.
PrCssUnit unitOf(const Source* s, uint32_t start, uint32_t end, CssTokenFlags flags) {
    PrStringView text = {s->text + start, end - start};
    if ((flags & CssTokenFlag_PlainText) != 0)
        return cssUnitFromText(text);

    char decoded[kCssUnitMaxLength];
    uint32_t size = cssValue(text, decoded, uint32_t(sizeof(decoded)));
    return size <= sizeof(decoded) ? cssUnitFromText(PrStringView{decoded, size}) : PrCssUnit_Unknown;
}

// Consumes a numeric token (css-syntax-3/#consume-numeric-token, css-syntax-3/#consume-number). The source must start
// with a number at `start`.
PR_NOINLINE Consumed consumeNumeric(const Source* s, uint32_t start) {
    Number n = readNumber(s, start);
    CssTokenFlags flags = numberFlags(&n);
    flags |= CssTokenFlag_PlainText;

    Consumed c;
    if (startsIdent(s, n.end, n.after)) {
        // The name adds to the flags, so it is consumed before they are read.
        uint32_t end = consumeName(s, n.end, &flags).pos;
        c = consumed(end, CssTokenKind_Dimension, flags);
        c.unit = unitOf(s, n.end, c.end, flags);
    } else if (n.after == '%') {
        c = consumed(n.end + 1, CssTokenKind_Percentage, flags);
    } else {
        c = consumed(n.end, CssTokenKind_Number, flags);
    }

    uint32_t unitEnd = c.kind == CssTokenKind_Dimension ? c.end : n.end;
    if ((c.flags & CssTokenFlag_PlainText) != 0 && numberIsVerbatim(s->text, start, &n, unitEnd))
        c.flags |= CssTokenFlag_Verbatim;
    return c;
}

// Consumes a string token (css-syntax-3/#consume-string-token). `start` is the opening quote, whose byte is `quote`.
PR_NOINLINE Consumed consumeString(const Source* s, uint32_t start, uint32_t quote) {
    const char* text = s->text;
    uint32_t size = s->size;
    ScanFlags stop = quote == '"' ? ScanFlag_Double : ScanFlag_Single;
    ScanFlags seen = 0;
    CssTokenFlags flags = CssTokenFlag_PlainText;
    CssTokenKind kind = CssTokenKind_String;
    bool closed = false;
    uint32_t pos = start + 1;
    for (;;) {
        uint32_t byte = kEnd;
        for (; pos < size; ++pos) {
            uint32_t next = uint8_t(text[pos]);
            ScanFlags classes = kScan.of[next];
            if ((classes & stop) != 0) {
                byte = next;
                break;
            }
            seen |= classes;
        }

        if (byte == kEnd) {
            flags |= CssTokenFlag_EofTerminated;
            break;
        }
        if (byte == quote) {
            ++pos;
            closed = true;
            break;
        }
        if (byte != '\\') {
            kind = CssTokenKind_BadString;   // at a newline, which is not part of the string
            break;
        }

        flags &= ~CssTokenFlag_PlainText;
        uint32_t after = at(s, pos + 1);
        if (after == kEnd) {
            ++pos;
            flags |= CssTokenFlag_EofTerminated;
            break;
        }
        if ((kCssByte.of[after] & CssByteFlag_Newline) != 0)
            pos += after == '\r' && at(s, pos + 2) == '\n' ? 3u : 2u;   // a line continuation
        else
            pos = consumeEscape(s, pos);
    }

    // A null or a byte to decode means that the bytes are not the value. A bad string serializes as its source
    // (css-syntax-3/#serialization). A string serializes as its bytes if it is double quoted, closed, its own value,
    // and free of control code points.
    if ((seen & ScanFlag_Decode) != 0)
        flags &= ~CssTokenFlag_PlainText;
    if (kind == CssTokenKind_BadString)
        flags |= CssTokenFlag_Verbatim;
    else if (closed && quote == '"' && (seen & ScanFlag_Control) == 0)
        flags = verbatimWhenPlain(flags);
    return consumed(pos, kind, flags);
}

// Consumes the remnants of a bad url from `pos` (css-syntax-3/#consume-remnants-of-bad-url). A bad url serializes as
// its source (css-syntax-3/#serialization).
Consumed badUrlRemnants(const Source* s, uint32_t pos, CssTokenFlags flags) {
    flags |= CssTokenFlag_Verbatim;
    for (;;) {
        uint32_t byte = at(s, pos);
        if (byte == kEnd)
            return consumed(pos, CssTokenKind_BadUrl, flags);
        if (byte == ')')
            return consumed(pos + 1, CssTokenKind_BadUrl, flags);
        pos = byte == '\\' && validEscape(s, pos) ? consumeEscape(s, pos) : pos + 1;
    }
}

// Consumes a url token (css-syntax-3/#consume-url-token). `pos` is past `url(` and the whitespace after it.
Consumed consumeUrl(const Source* s, uint32_t pos, CssTokenFlags flags) {
    for (;;) {
        uint32_t byte = at(s, pos);
        if (byte == kEnd) {
            flags |= CssTokenFlag_EofTerminated;
            return consumed(pos, CssTokenKind_Url, flags);
        }
        if (byte == ')')
            return consumed(pos + 1, CssTokenKind_Url, flags);

        CssByteFlags classes = kCssByte.of[byte];
        if ((classes & CssByteFlag_Space) != 0) {
            pos = skipSpace(s, pos + 1);
            uint32_t after = at(s, pos);
            if (after == kEnd) {
                flags |= CssTokenFlag_EofTerminated;
                return consumed(pos, CssTokenKind_Url, flags);
            }
            if (after == ')')
                return consumed(pos + 1, CssTokenKind_Url, flags);
            return badUrlRemnants(s, pos, flags);
        }
        if (byte == '\\') {
            if (!validEscape(s, pos))
                return badUrlRemnants(s, pos, flags);
            flags &= ~CssTokenFlag_PlainText;
            pos = consumeEscape(s, pos);
            continue;
        }
        if ((classes & CssByteFlag_UrlEnd) != 0)
            return badUrlRemnants(s, pos, flags);   // a quote, a '(' or a non-printable
        if ((classes & CssByteFlag_Decode) != 0)
            flags &= ~CssTokenFlag_PlainText;
        ++pos;
    }
}

// Returns true if the ident sequence from `start` to `end` is `url`, in any case, with escapes resolved.
bool isUrl(const Source* s, uint32_t start, uint32_t end, CssTokenFlags flags) {
    PrStringView text = {s->text + start, end - start};
    if ((flags & CssTokenFlag_PlainText) != 0)
        return prAsciiEqualsFold(text, "url");

    char name[3];
    return cssValue(text, name, 3) == 3 && prAsciiEqualsFold(PrStringView{name, 3}, "url");
}

// Consumes the function or url token that the ident sequence from `start` to `end` starts, with the '(' at `end`
// (css-syntax-3/#consume-ident-like-token).
PR_NOINLINE Consumed consumeFunction(const Source* s, uint32_t start, uint32_t end, CssTokenFlags flags) {
    if (!isUrl(s, start, end, flags))
        return consumed(end + 1, CssTokenKind_Function, verbatimWhenPlain(flags));

    // url( x ) is a url token. url( "x" ) is a function whose argument is a string.
    uint32_t body = skipSpace(s, end + 1);
    if (kLead.of[at(s, body)] == Lead_Quote)
        return consumed(end + 1, CssTokenKind_Function, verbatimWhenPlain(flags));
    return consumeUrl(s, body, flags);
}

// Consumes an ident-like token at `start`, whose bytes before `pos` are ASCII name bytes
// (css-syntax-3/#consume-ident-like-token). Writes the byte after the token, or kEnd, to `next`.
PR_FORCE_INLINE Consumed consumeIdentLike(const Source* s, uint32_t start, uint32_t pos, uint32_t* next) {
    CssTokenFlags flags = CssTokenFlag_PlainText;
    NameEnd name = consumeName(s, pos, &flags);
    if (name.byte != '(') {
        *next = name.byte;
        return consumed(name.pos, CssTokenKind_Ident, verbatimWhenPlain(flags));
    }
    Consumed c = consumeFunction(s, start, name.pos, flags);
    *next = at(s, c.end);
    return c;
}

// Consumes an ident-like token in the same way, without reporting the byte after the token.
Consumed consumeIdentLike(const Source* s, uint32_t start, uint32_t pos) {
    uint32_t next = 0;
    return consumeIdentLike(s, start, pos, &next);
}

// Consumes a unicode-range token (css-syntax-3/#consume-unicode-range-token). Returns the position one past the unicode
// range at `pos`, and writes the code points that it covers. Question marks in the first run of digits stand for the
// low and high ends of the range. Without them, a '-' and more digits give the end. With neither, the range is one code
// point wide.
uint32_t unicodeRangeEnd(const Source* s, uint32_t pos, uint32_t* first, uint32_t* last) {
    pos += 2;                                   // `u+`

    uint32_t low = 0;
    uint32_t digits = 0;
    for (; digits < 6 && prAsciiIsHexDigit(char(at(s, pos))); ++digits) {
        low = (low << 4) | prAsciiHexValue(s->text[pos]);
        ++pos;
    }

    uint32_t marks = 0;
    for (; digits + marks < 6 && at(s, pos) == '?'; ++marks) {
        low <<= 4;
        ++pos;
    }

    *first = low;
    *last = low;
    if (marks != 0) {
        for (uint32_t i = 0; i < marks; ++i)
            *last |= 0xfu << (i * 4);
        return pos;
    }

    if (at(s, pos) != '-' || !prAsciiIsHexDigit(char(at(s, pos + 1))))
        return pos;

    ++pos;
    uint32_t high = 0;
    for (uint32_t i = 0; i < 6 && prAsciiIsHexDigit(char(at(s, pos))); ++i) {
        high = (high << 4) | prAsciiHexValue(s->text[pos]);
        ++pos;
    }
    *last = high;
    return pos;
}

// Consumes a run of whitespace from `pos`, whose first byte is `byte`. The run serializes as one space. Its bytes are
// its value unless preprocessing rewrites one of them. Writes the byte after the run, or kEnd, to `next`.
PR_FORCE_INLINE Consumed consumeWhitespace(const Source* s, uint32_t pos, uint32_t byte, uint32_t* next) {
    const char* text = s->text;
    uint32_t size = s->size;
    CssByteFlags seen = kCssByte.of[byte];
    uint32_t end = pos + 1;
    *next = kEnd;
    for (; end < size; ++end) {
        uint32_t after = uint8_t(text[end]);
        CssByteFlags classes = kCssByte.of[after];
        if ((classes & CssByteFlag_Space) == 0) {
            *next = after;
            break;
        }
        seen |= classes;
    }

    CssTokenFlags flags = (seen & CssByteFlag_Rewritten) == 0 ? CssTokenFlag_PlainText : 0;
    if (end - pos == 1 && byte == ' ')
        flags |= CssTokenFlag_Verbatim;
    return consumed(end, CssTokenKind_Whitespace, flags);
}

// Consumes a '#' at `pos`: a hash token if a name follows it, and a delim otherwise. A digit or a '-' continues a name
// without starting an ident sequence, so a hash that starts with one is not an id.
PR_NOINLINE Consumed consumeHash(const Source* s, uint32_t pos) {
    uint32_t after = at(s, pos + 1);
    CssTokenFlags flags = CssTokenFlag_PlainText;
    if (startsIdent(s, pos + 1, after))
        flags |= CssTokenFlag_Id;
    else if (kLead.of[after] != Lead_Digit && kLead.of[after] != Lead_Hyphen)
        return delimAt(pos);
    NameEnd name = consumeName(s, pos + 1, &flags);
    return consumed(name.pos, CssTokenKind_Hash, verbatimWhenPlain(flags));
}

// Consumes a '-' at `pos` whose next byte, `second`, is neither a '-' nor an ident-start byte. The result is a number,
// an ident-like token that starts with an escape or a code point above ASCII, or a delim.
PR_NOINLINE Consumed consumeHyphen(const Source* s, uint32_t pos, uint32_t second) {
    if (numberAfterSign(s, pos, second))
        return consumeNumeric(s, pos);
    if (identStart(s, pos + 1, second))
        return consumeIdentLike(s, pos, pos + 1);
    return delimAt(pos);
}

// Consumes an '@' at `pos`: an at-keyword if a name follows it, and a delim otherwise.
PR_NOINLINE Consumed consumeAt(const Source* s, uint32_t pos) {
    if (!startsIdent(s, pos + 1, at(s, pos + 1)))
        return delimAt(pos);
    CssTokenFlags flags = CssTokenFlag_PlainText;
    NameEnd name = consumeName(s, pos + 1, &flags);
    return consumed(name.pos, CssTokenKind_AtKeyword, verbatimWhenPlain(flags));
}

// Consumes a unicode range at `pos`, which starts with `u+`.
PR_NOINLINE Consumed consumeUnicodeRange(const Source* s, uint32_t pos) {
    uint32_t first = 0;
    uint32_t last = 0;
    return consumed(unicodeRangeEnd(s, pos, &first, &last), CssTokenKind_UnicodeRange,
        CssTokenFlag_PlainText);   // not Verbatim: a range serializes in hex
}

// Returns the position one past the `*/` that closes the comment whose body begins at `pos`, or the end of the source
// (css-syntax-3/#consume-comment).
PR_NOINLINE uint32_t commentEnd(const Source* s, uint32_t pos) {
    const char* text = s->text;
    uint32_t size = s->size;
    bool star = false;
    for (; pos < size; ++pos) {
        uint32_t byte = uint8_t(text[pos]);
        if (star && byte == '/')
            return pos + 1;
        star = byte == '*';
    }
    return pos;
}

// Consumes a token that the token loop does not consume itself (css-syntax-3/#consume-token). The token's first byte,
// `byte`, is a digit, a quote, a '#', a '+', a '<', an '@', a backslash, a null or a byte above ASCII.
PR_NOINLINE Consumed consumeToken(const Source* s, uint32_t pos, uint32_t byte) {
    switch (kLead.of[byte]) {
        case Lead_Digit:
            return consumeNumeric(s, pos);
        case Lead_Quote:
            return consumeString(s, pos, byte);
        case Lead_Hash:
            return consumeHash(s, pos);
        case Lead_Plus:
            return numberAfterSign(s, pos, at(s, pos + 1)) ? consumeNumeric(s, pos) : delimAt(pos);
        case Lead_Less:
            if (at(s, pos + 1) == '!' && at(s, pos + 2) == '-' && at(s, pos + 3) == '-')
                return consumed(pos + 4, CssTokenKind_Cdo, CssTokenFlag_PlainVerbatim);
            return delimAt(pos);
        case Lead_At:
            return consumeAt(s, pos);
        case Lead_Backslash:
            if (validEscape(s, pos))
                return consumeIdentLike(s, pos, pos);
            // Not Verbatim: the delim serializes escaped.
            return consumed(pos + 1, CssTokenKind_Delim, CssTokenFlag_PlainText);
        default:
            return consumeIdentLike(s, pos, pos);   // a null or a byte above ASCII
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The token loop

// Returns true if a token of the kind opens a block, or closes one.
constexpr bool opensBlock(CssTokenKind kind) {
    return kind == CssTokenKind_Function || kind == CssTokenKind_OpenParen ||
        kind == CssTokenKind_OpenBracket || kind == CssTokenKind_OpenBrace;
}

constexpr bool closesBlock(CssTokenKind kind) {
    return kind == CssTokenKind_CloseParen || kind == CssTokenKind_CloseBracket || kind == CssTokenKind_CloseBrace;
}

// The kinds that open or close a block, one bit each.
constexpr uint32_t makeBracketKinds() {
    uint32_t mask = 0;
    for (CssTokenKind kind = 0; kind < CssTokenKind_Count; ++kind) {
        if (opensBlock(kind) || closesBlock(kind))
            mask |= 1u << kind;
    }
    return mask;
}

constexpr uint32_t kBracketKinds = makeBracketKinds();

// Pairs the close token at `index` with the opener that it closes, or marks it unmatched. Records an opener as waiting
// in `open`, innermost last.
void pairBracket(CssTokenArray* tokens, Array<uint32_t>* open, uint32_t index, CssTokenKind kind) {
    if (opensBlock(kind)) {
        open->push(index);
        return;
    }

    // css-syntax-3/#parse-component-value ends a block at a close token of the kind it opened with, so a close token of
    // another kind closes nothing.
    uint32_t depth = open->count;
    if (depth != 0) {
        uint32_t opener = open->data[depth - 1];
        CssTokenKind opened = tokens->kinds.data[opener];
        bool closes = (kind == CssTokenKind_CloseParen &&
            (opened == CssTokenKind_OpenParen || opened == CssTokenKind_Function)) ||
            (kind == CssTokenKind_CloseBracket && opened == CssTokenKind_OpenBracket) ||
            (kind == CssTokenKind_CloseBrace && opened == CssTokenKind_OpenBrace);
        if (closes) {
            tokens->closers.data[opener] = index;
            open->count = depth - 1;
            return;
        }
    }
    tokens->flags.data[index] |= CssTokenFlag_Unmatched;
}

// How much of the source one commit of the lanes covers.
constexpr uint32_t kCommitSourceStep = uint32_t(kVmCommitStep);

// Commits every lane for the tokens that start before the next limit, one step past `pos`, and the entry after them. A
// token's index never exceeds its start position, because every token before it read at least one byte. Returns the
// position at which the next commit is due.
uint32_t commitLanes(CssTokenArray* tokens, uint32_t pos, uint32_t size) {
    uint32_t limit = size - pos > kCommitSourceStep ? pos + kCommitSourceStep : size;
    tokens->starts.commitTo(limit + 1);
    tokens->ends.commitTo(limit + 1);
    tokens->kinds.commitTo(limit + 1);
    tokens->flags.commitTo(limit + 1);
    tokens->units.commitTo(limit + 1);
    tokens->closers.commitTo(limit + 1);
    return limit;
}

// Writes every token of the source to `tokens`, committing the lanes one step of the source ahead of the tokens
// written. `ranges` allows unicode ranges (css-syntax-3/#consume-token). Returns the number of tokens.
uint32_t tokenizeAll(CssTokenArray* tokens, Array<uint32_t>* open, const Source* s, bool ranges) {
    uint32_t size = s->size;
    uint32_t* starts = tokens->starts.data;
    uint32_t* ends = tokens->ends.data;
    CssTokenKind* kinds = tokens->kinds.data;
    CssTokenFlags* flags = tokens->flags.data;
    PrCssUnit* units = tokens->units.data;

    uint32_t pos = 0;
    uint32_t count = 0;
    CssTokenFlags contiguous = CssTokenFlag_Contiguous;   // zero after a comment
    uint32_t byte = at(s, 0);
    while (pos < size) {
        uint32_t limit = commitLanes(tokens, pos, size);
        while (pos < limit) {
            Consumed c;
            uint32_t next;
            switch (kLead.of[byte]) {
                case Lead_Single:
                    c = consumed(pos + 1, kCssPunctuation.kindOf[byte], CssTokenFlag_PlainVerbatim);
                    next = at(s, pos + 1);
                    break;
                case Lead_Space:
                    c = consumeWhitespace(s, pos, byte, &next);
                    break;
                case Lead_Name:
                    if (ranges && startsUnicodeRange(s, pos, byte)) {
                        c = consumeUnicodeRange(s, pos);
                        next = at(s, c.end);
                        break;
                    }
                    c = consumeIdentLike(s, pos, pos + 1, &next);
                    break;
                case Lead_Hyphen: {
                    uint32_t second = at(s, pos + 1);
                    Lead lead = kLead.of[second];
                    if (lead != Lead_Hyphen && lead != Lead_Name) {
                        c = consumeHyphen(s, pos, second);
                        next = at(s, c.end);
                        break;
                    }
                    c = consumeIdentLike(s, pos, pos + 2, &next);

                    // Two hyphens and a '>' form a CDC, not an ident and a delim.
                    if (lead == Lead_Hyphen && c.end == pos + 2 && next == '>') {
                        c = consumed(pos + 3, CssTokenKind_Cdc, CssTokenFlag_PlainVerbatim);
                        next = at(s, pos + 3);
                    }
                    break;
                }
                case Lead_Dot:
                    next = at(s, pos + 1);
                    if (!prAsciiIsDigit(char(next))) {
                        c = delimAt(pos);
                        break;
                    }
                    c = consumeNumeric(s, pos);
                    next = at(s, c.end);
                    break;
                case Lead_Slash:
                    next = at(s, pos + 1);
                    if (next != '*') {
                        c = delimAt(pos);
                        break;
                    }
                    // A comment is not a token (css-syntax-3/#consume-comment).
                    pos = commentEnd(s, pos + 2);
                    byte = at(s, pos);
                    contiguous = 0;
                    continue;
                default:
                    c = consumeToken(s, pos, byte);
                    next = at(s, c.end);
                    break;
            }

            starts[count] = pos;
            ends[count] = c.end;
            kinds[count] = c.kind;
            flags[count] = c.flags | contiguous;
            units[count] = c.unit;
            if (((kBracketKinds >> c.kind) & 1) != 0)
                pairBracket(tokens, open, count, c.kind);
            ++count;
            contiguous = CssTokenFlag_Contiguous;
            pos = c.end;
            byte = next;
        }
    }
    commitLanes(tokens, size, size);
    return count;
}

// ---------------------------------------------------------------------------------------------------------------------
// Component values (css-syntax-3/#parsing)

// Returns true if `span` holds only what a <declaration-value> may hold (css-syntax-3/#parse-list-of-component-values).
// It must have no bad string, no bad url, and no close token that closes nothing, as marked by the tokenizer during its
// scan.
bool isDeclarationValue(const CssTokenStream* ts, CssTokenSpan span) {
    for (uint32_t i = span.start; i < span.start + span.count; ++i) {
        CssTokenKind kind = cssTokenKind(ts->tokens, i);
        if (kind == CssTokenKind_BadString || kind == CssTokenKind_BadUrl)
            return false;
        if ((cssTokenFlags(ts->tokens, i) & CssTokenFlag_Unmatched) != 0)
            return false;
    }
    return true;
}

bool consumeValue(CssTokenStream* ts, CssTokenSpan* out, bool stopAtSemicolon) {
    uint32_t start = ts->pos;
    for (;;) {
        CssTokenKind kind = cssPeek(ts);
        if (kind == CssTokenKind_Eof)
            break;
        if (stopAtSemicolon) {
            if (kind == CssTokenKind_Semicolon)
                break;
            if (kind == CssTokenKind_Delim && cssTokenDelim(ts->tokens, ts->pos) == '!')
                break;
        }
        // A closer that this run did not open belongs to the run that opened it.
        if (closesBlock(kind))
            break;

        CssTokenSpan ignored;
        if (!cssConsumeComponentValue(ts, &ignored))
            break;
    }

    out->start = start;
    out->count = ts->pos - start;
    if (out->count == 0)
        return false;

    if (isDeclarationValue(ts, *out))
        return true;

    cssReset(ts, start);
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// An+B (css-syntax-3/#anb-microsyntax)

bool isIdentKeyword(const CssTokenStream* ts, PrCssKeyword keyword) {
    return cssPeek(ts) == CssTokenKind_Ident && cssTokenKeyword(ts->tokens, ts->pos) == keyword;
}

bool isIntegerNumber(const CssTokenStream* ts) {
    return cssPeek(ts) == CssTokenKind_Number && (cssTokenFlags(ts->tokens, ts->pos) & CssTokenFlag_Integer) != 0;
}

bool isSigned(const CssTokenStream* ts) {
    return (cssTokenFlags(ts->tokens, ts->pos) & CssTokenFlag_SignPresent) != 0;
}

// Returns the integer value of a number token, clamped to the range of an int32_t. An+B always reads it as written.
int32_t integerValue(const CssTokenStream* ts) {
    return prF64SaturateI32(cssTokenNumber(ts->tokens, ts->pos));
}

// Matches "n", "n-", or "n-<digits>", which are the shapes of a dimension's unit or an ident. Writes the digits,
// negated and clamped to the range of an int32_t, to `trailing`. Writes whether there were any digits to `hasTrailing`.
bool matchesN(PrStringView text, uint32_t skip, int32_t* trailing, bool* hasTrailing) {
    uint32_t i = skip;
    if (i >= text.size || prAsciiLower(text.data[i]) != 'n')
        return false;
    ++i;

    *trailing = 0;
    *hasTrailing = false;
    if (i == text.size)
        return true;                       // "n"
    if (text.data[i] != '-')
        return false;
    ++i;
    if (i == text.size)
        return true;                       // "n-", whose digits follow as a token

    uint64_t value = 0;
    if (!prU64Parse(text.data + i, text.size - i, &value, 10))
        return false;
    *trailing = value < 0x80000000 ? -int32_t(value) : INT32_MIN;
    *hasTrailing = true;
    return true;
}

// Reads what follows the `a` part: an optional signed integer, or a sign and an unsigned integer.
void readTail(CssTokenStream* ts, int32_t* b) {
    uint32_t mark = cssMark(ts);
    cssSkipWhitespace(ts);

    if (isIntegerNumber(ts) && isSigned(ts)) {
        *b = integerValue(ts);
        ++ts->pos;
        return;
    }

    int32_t sign = 0;
    if (cssPeekDelim(ts, '+'))
        sign = 1;
    else if (cssPeekDelim(ts, '-'))
        sign = -1;
    if (sign != 0) {
        ++ts->pos;
        cssSkipWhitespace(ts);
        if (isIntegerNumber(ts) && !isSigned(ts)) {
            *b = sign * integerValue(ts);
            ++ts->pos;
            return;
        }
    }

    cssReset(ts, mark);
    *b = 0;
}

// Reads the digits of an "n-" from the following token, which must be unsigned.
bool readTrailingSignless(CssTokenStream* ts, int32_t* b) {
    cssSkipWhitespace(ts);
    if (!isIntegerNumber(ts) || isSigned(ts))
        return false;
    *b = -integerValue(ts);
    ++ts->pos;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Tokenizing

void cssTokenize(CssTokenArray* tokens, CssTokenScratch* scratch, PrStringView source, CssTokenizeFlags flags) {
    PR_ASSERT(source.size < 0xffffffffu);
    uint32_t size = uint32_t(source.size);

    uint32_t entries = size + 1;
    tokens->starts.prepare(entries);
    tokens->ends.prepare(entries);
    tokens->kinds.prepare(entries);
    tokens->flags.prepare(entries);
    tokens->units.prepare(entries);
    tokens->closers.prepare(entries);
    tokens->source = source;
    tokens->values = nullptr;
    tokens->valueText = nullptr;

    Source reader = {source.data, size};
    scratch->open.count = 0;
    uint32_t count = tokenizeAll(tokens, &scratch->open, &reader, (flags & CssTokenizeFlag_UnicodeRanges) != 0);

    // The entry past the last token, and the blocks that the input never closed, which end at the end of the input.
    tokens->starts.data[count] = size;
    tokens->ends.data[count] = size;
    tokens->kinds.data[count] = CssTokenKind_Eof;
    tokens->flags.data[count] = 0;
    tokens->units.data[count] = PrCssUnit_Number;
    tokens->closers.data[count] = count;
    for (uint32_t i = 0; i < scratch->open.count; ++i)
        tokens->closers.data[scratch->open.data[i]] = count;

    tokens->starts.count = count;
    tokens->ends.count = count;
    tokens->kinds.count = count;
    tokens->flags.count = count;
    tokens->units.count = count;
    tokens->closers.count = count;
}

void cssTokenScratchReset(CssTokenScratch* scratch) {
    scratch->open.reset();
}

void cssTokenArrayReset(CssTokenArray* tokens) {
    tokens->starts.reset();
    tokens->ends.reset();
    tokens->kinds.reset();
    tokens->flags.reset();
    tokens->units.reset();
    tokens->closers.reset();
    tokens->source = {};
    tokens->values = nullptr;
    tokens->valueText = nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Reading a token

PrStringView cssTokenValueText(const CssTokenArray* tokens, uint32_t index) {
    uint32_t start = tokens->starts.data[index];
    uint32_t end = tokens->ends.data[index];
    const char* text = tokens->source.data;

    switch (tokens->kinds.data[index]) {
        case CssTokenKind_Function:
            --end;                                  // the '('
            break;
        case CssTokenKind_AtKeyword:
        case CssTokenKind_Hash:
            ++start;                                // the '@' or the '#'
            break;
        case CssTokenKind_String: {
            // The closing quote is missing if the string ran to the end of the input.
            ++start;
            if ((tokens->flags.data[index] & CssTokenFlag_EofTerminated) == 0)
                --end;

            // css-syntax-3/#consume-string-token checks for the end of input before consuming an escape, so a string
            // ending in a backslash loses it. Everywhere else, css-syntax-3/#consume-escaped-code-point turns the same
            // backslash into U+FFFD.
            uint32_t back = end;
            while (back > start && text[back - 1] == '\\')
                --back;
            if (((end - back) & 1) != 0)
                --end;
            break;
        }
        case CssTokenKind_Url:
            while (text[start] != '(')              // url( however its name was spelled
                ++start;
            ++start;
            if ((tokens->flags.data[index] & CssTokenFlag_EofTerminated) == 0 && end > start)
                --end;                              // the ')'
            return stringTrimBy(PrStringView{text + start, end - start}, [](char c) {
                return (kCssByte.of[uint8_t(c)] & CssByteFlag_Space) != 0;
            });
        default:
            break;
    }
    return PrStringView{text + start, end - start};
}

bool cssValueNext(PrStringView text, uint32_t* pos, uint32_t* codePoint) {
    Source s = {text.data, uint32_t(text.size)};
    while (*pos < s.size) {
        uint32_t here = *pos;
        if (s.text[here] != '\\') {
            uint32_t width = decodeAt(&s, here, codePoint);
            if (*codePoint == '\r') {
                *codePoint = '\n';
                width = at(&s, here + 1) == '\n' ? 2 : 1;
            } else if (*codePoint == '\f') {
                *codePoint = '\n';
            }
            *pos = here + width;
            return true;
        }

        uint32_t after = at(&s, here + 1);
        if (after == kEnd) {
            *pos = s.size;
            *codePoint = kPrUtf8Replacement;   // a trailing backslash spells U+FFFD
            return true;
        }
        if ((kCssByte.of[after] & CssByteFlag_Newline) != 0) {
            *pos = here + (after == '\r' && at(&s, here + 2) == '\n' ? 3u : 2u);
            continue;                           // a line continuation
        }
        if (!prAsciiIsHexDigit(char(after))) {
            *pos = here + 1 + decodeAt(&s, here + 1, codePoint);
            return true;
        }

        // The value is the escape's hex digits, which end at the whitespace after them.
        uint32_t end = consumeEscape(&s, here);
        uint32_t value = 0;
        for (uint32_t i = here + 1; i < end && prAsciiIsHexDigit(s.text[i]); ++i)
            value = (value << 4) | prAsciiHexValue(s.text[i]);
        *pos = end;
        *codePoint = value != 0 ? value : kPrUtf8Replacement;
        return true;
    }
    return false;
}

uint32_t cssValue(PrStringView text, char* PR_NULLABLE buffer, uint32_t capacity) {
    uint32_t written = 0;
    uint32_t pos = 0;
    uint32_t codePoint = 0;
    while (cssValueNext(text, &pos, &codePoint)) {
        char encoded[kPrUtf8MaxBytes];
        uint32_t size = prUtf8Encode(codePoint, encoded);
        for (uint32_t i = 0; i < size; ++i) {
            if (written + i < capacity)
                buffer[written + i] = encoded[i];
        }
        written += size;
    }
    return written;
}

void cssValueAppend(PrTextBuffer* out, PrStringView text) {
    uint32_t pos = 0;
    uint32_t codePoint = 0;
    while (cssValueNext(text, &pos, &codePoint))
        prTextBufferAppendCodePoint(out, codePoint);
}

PrCssKeyword cssTokenKeyword(const CssTokenArray* tokens, uint32_t index) {
    PrStringView text = cssTokenValueText(tokens, index);
    if ((tokens->flags.data[index] & CssTokenFlag_PlainText) != 0)
        return cssKeywordFromText(text);

    char decoded[kCssKeywordMaxLength];
    uint32_t size = cssValue(text, decoded, uint32_t(countOf(decoded)));
    if (size > countOf(decoded))
        return PrCssKeyword_Unknown;   // longer than any keyword
    return cssKeywordFromText(PrStringView{decoded, size});
}

bool cssTokenIsDashedIdent(const CssTokenArray* tokens, uint32_t index) {
    if (tokens->kinds.data[index] != CssTokenKind_Ident)
        return false;

    PrStringView text = cssTokenValueText(tokens, index);
    if ((tokens->flags.data[index] & CssTokenFlag_PlainText) != 0)
        return prStringStartsWith(text, "--");

    char decoded[2];
    return cssValue(text, decoded, uint32_t(sizeof(decoded))) >= 2 && decoded[0] == '-' && decoded[1] == '-';
}

double cssTokenNumber(const CssTokenArray* tokens, uint32_t index) {
    uint32_t start = tokens->starts.data[index];
    uint32_t end = tokens->ends.data[index];

    Source s = {tokens->source.data, end};
    return prF64Parse(tokens->source.data + start, readNumber(&s, start).end - start);
}

PrStringView cssTokenUnitText(const CssTokenArray* tokens, uint32_t index) {
    uint32_t end = tokens->ends.data[index];
    Source s = {tokens->source.data, end};
    uint32_t unit = readNumber(&s, tokens->starts.data[index]).end;
    return PrStringView{tokens->source.data + unit, end - unit};
}

void cssTokenRange(const CssTokenArray* tokens, uint32_t index, uint32_t* first, uint32_t* last) {
    Source s = {tokens->source.data, tokens->ends.data[index]};
    unicodeRangeEnd(&s, tokens->starts.data[index], first, last);
}

bool cssPeekDelim(const CssTokenStream* ts, uint32_t codePoint) {
    return cssPeek(ts) == CssTokenKind_Delim && cssTokenDelim(ts->tokens, ts->pos) == codePoint;
}

// Returns true if only whitespace is left.
bool cssAtEnd(CssTokenStream* ts) {
    cssSkipWhitespace(ts);
    return cssPeek(ts) == CssTokenKind_Eof;
}

// Finds the next comma at this level, if the run holds one. A '{', or a closer that this run did not open, ends the
// run.
bool cssNextComma(const CssTokenStream* ts, uint32_t* out) {
    CssTokenStream scan = *ts;
    while (scan.pos < scan.end) {
        switch (cssPeek(&scan)) {
            case CssTokenKind_Comma:
                *out = scan.pos;
                return true;
            case CssTokenKind_CloseParen:
            case CssTokenKind_CloseBracket:
            case CssTokenKind_CloseBrace:
            case CssTokenKind_OpenBrace:
                return false;
            default:
                break;
        }
        CssTokenSpan ignored;
        if (!cssConsumeComponentValue(&scan, &ignored))
            return false;
    }
    return false;
}

// Returns the end of the block that the stream is in: the first closer that the stream did not open, or the stream's
// end.
uint32_t cssBlockEnd(const CssTokenStream* ts) {
    CssTokenStream scan = *ts;
    while (scan.pos < scan.end) {
        CssTokenKind kind = cssPeek(&scan);
        if (kind == CssTokenKind_CloseParen || kind == CssTokenKind_CloseBracket || kind == CssTokenKind_CloseBrace)
            break;
        CssTokenSpan ignored;
        cssConsumeComponentValue(&scan, &ignored);
    }
    return scan.pos;
}

CssTokenSpan cssTrimWhitespace(const CssTokenArray* tokens, CssTokenSpan span) {
    while (span.count != 0 && tokens->kinds.data[span.start] == CssTokenKind_Whitespace) {
        ++span.start;
        --span.count;
    }
    while (span.count != 0 && tokens->kinds.data[span.start + span.count - 1] == CssTokenKind_Whitespace)
        --span.count;
    return span;
}

PrStringView cssSpanText(const CssTokenArray* tokens, CssTokenSpan span) {
    uint32_t start = tokens->starts.data[span.start];
    uint32_t end = span.count != 0 ? tokens->ends.data[span.start + span.count - 1] : start;
    return PrStringView{tokens->source.data + start, end - start};
}

bool cssSpanIsVerbatim(const CssTokenArray* tokens, CssTokenSpan span, PrStringView* out) {
    if (span.count == 0) {
        *out = PrStringView{tokens->source.data, 0};
        return true;
    }

    // The first token starts the run, so the tokens before it do not matter.
    const CssTokenFlags kRun = CssTokenFlag_Verbatim | CssTokenFlag_Contiguous;
    CssTokenFlags all = tokens->flags.data[span.start];
    all |= CssTokenFlag_Contiguous;
    for (uint32_t i = 1; i < span.count; ++i)
        all &= tokens->flags.data[span.start + i];
    if ((all & kRun) != kRun)
        return false;

    *out = cssSpanText(tokens, span);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Component values

bool cssConsumeComponentValue(CssTokenStream* ts, CssTokenSpan* out) {
    uint32_t start = ts->pos;
    CssTokenKind kind = cssPeek(ts);
    if (kind == CssTokenKind_Eof)
        return false;

    if (!opensBlock(kind)) {
        ts->pos = start + 1;
    } else {
        uint32_t end = cssTokenBlockEnd(ts->tokens, start);
        ts->pos = end < ts->end ? end : ts->end;
    }

    out->start = start;
    out->count = ts->pos - start;
    return true;
}

bool cssConsumeDeclarationValue(CssTokenStream* ts, CssTokenSpan* out) {
    return consumeValue(ts, out, true);
}

bool cssConsumeAnyValue(CssTokenStream* ts, CssTokenSpan* out) {
    return consumeValue(ts, out, false);
}

// ---------------------------------------------------------------------------------------------------------------------
// An+B

bool cssConsumeAnPlusB(CssTokenStream* ts, int32_t* a, int32_t* b) {
    uint32_t mark = cssMark(ts);

    if (isIdentKeyword(ts, PrCssKeyword_Odd)) {
        ++ts->pos;
        *a = 2;
        *b = 1;
        return true;
    }
    if (isIdentKeyword(ts, PrCssKeyword_Even)) {
        ++ts->pos;
        *a = 2;
        *b = 0;
        return true;
    }

    // A '+' counts only if it touches what follows: "+ n" is not "+n".
    if (cssPeekDelim(ts, '+')) {
        if ((cssTokenFlags(ts->tokens, ts->pos + 1) & CssTokenFlag_Contiguous) == 0) {
            cssReset(ts, mark);
            return false;
        }
        ++ts->pos;
    }

    if (isIntegerNumber(ts)) {
        *a = 0;
        *b = integerValue(ts);
        ++ts->pos;
        return true;
    }

    // The n, and what follows it in the same token. The n is a dimension's unit after its integer, or an ident with a
    // '-' before the n for a coefficient of -1.
    CssTokenKind kind = cssPeek(ts);
    PrStringView text;
    uint32_t skip = 0;
    int32_t coefficient = 1;
    bool integer = (cssTokenFlags(ts->tokens, ts->pos) & CssTokenFlag_Integer) != 0;
    if (kind == CssTokenKind_Dimension && integer) {
        text = cssTokenUnitText(ts->tokens, ts->pos);
        coefficient = integerValue(ts);
    } else if (kind == CssTokenKind_Ident) {
        text = cssTokenText(ts->tokens, ts->pos);
        if (prStringStartsWith(text, "-")) {
            coefficient = -1;
            skip = 1;
        }
    } else {
        cssReset(ts, mark);
        return false;
    }

    int32_t trailing = 0;
    bool hasTrailing = false;
    if (!matchesN(text, skip, &trailing, &hasTrailing)) {
        cssReset(ts, mark);
        return false;
    }
    *a = coefficient;
    ++ts->pos;

    if (hasTrailing) {
        *b = trailing;
        return true;
    }
    if (text.size - skip == 2) {
        // "n-" takes its digits from the next token.
        if (readTrailingSignless(ts, b))
            return true;
        cssReset(ts, mark);
        return false;
    }
    readTail(ts, b);
    return true;
}

} // namespace pr_impl
