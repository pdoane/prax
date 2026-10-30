// Tokenization
//
// html/#tokenization - https://html.spec.whatwg.org/multipage/parsing.html#tokenization

#include "html/src/html_tokenizer.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/html_entity.h"
#include "pr/mem.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/utf8.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

extern const PrStringView kHtmlStateName[HtmlState_Count] = {
    "data", "rcdata", "rawtext", "script-data", "plaintext", "cdata-section", "script-data-escaped",
        "script-data-double-escaped",
};

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// The input stream (html/#preprocessing-the-input-stream)

// The value read at the end of the input. No byte has this value.
constexpr uint32_t kEof = 0xffffffffu;

// U+FFFD REPLACEMENT CHARACTER, which several states write in place of what they read.
constexpr PrStringView kReplacement = "\xef\xbf\xbd";

// Newline normalization (html/#preprocessing-the-input-stream), the only rewriting done in the tokenizer's copy. Writes
// `text` to `out` with every carriage return, and every carriage return line feed pair, replaced by one line feed.
// Returns the number of bytes written. `out` has room for text.size bytes.
uint32_t normalizeNewlines(char* out, PrStringView text) {
    char* write = out;
    size_t at = 0;
    while (at < text.size) {
        const char* PR_NULLABLE mark = memFind(text.data + at, '\r', text.size - at);
        size_t end = mark != nullptr ? size_t(mark - text.data) : text.size;
        memCopy(write, text.data + at, end - at);
        write += end - at;
        at = end;
        if (mark == nullptr)
            break;

        *write++ = '\n';
        at += at + 1 < text.size && text.data[at + 1] == '\n' ? 2 : 1;
    }
    return uint32_t(write - out);
}

// Returns `text` as the input stream holds it, with U+FFFD in place of each ill-formed sequence
// (encoding/#utf-8-decoder). Text that needs no replacement is returned unchanged. Otherwise the result is written to
// `repaired`.
PrStringView wellFormed(PrStringView text, TextBuffer* repaired) {
    if (prUtf8Validate(text) == text.size)
        return text;
    textAppendUtf8(repaired, text);
    return textView(repaired);
}

constexpr bool isSurrogate(uint32_t codePoint) {
    return codePoint >= 0xd800 && codePoint <= 0xdfff;
}

constexpr bool isNoncharacter(uint32_t codePoint) {
    return (codePoint >= 0xfdd0 && codePoint <= 0xfdef) || (codePoint & 0xfffe) == 0xfffe;
}

// Returns true if the code point is a control that html/#preprocessing-the-input-stream reports, which is one that is
// neither ASCII whitespace nor a null.
constexpr bool isControl(uint32_t codePoint) {
    if (codePoint >= 0x7f)
        return codePoint <= 0x9f;
    return codePoint != 0 && codePoint < 0x20 && codePoint != '\t' && codePoint != '\n' &&
        codePoint != '\f' && codePoint != '\r';
}

// Returns the code point that html/#numeric-character-reference-end-state substitutes for a numeric character reference
// that names one of the C1 controls. It is the code point that a windows-1252 decoder would have produced for that
// byte.
uint32_t numericReplacement(uint32_t code) {
    switch (code) {
        case 0x80: return 0x20ac;
        case 0x82: return 0x201a;
        case 0x83: return 0x0192;
        case 0x84: return 0x201e;
        case 0x85: return 0x2026;
        case 0x86: return 0x2020;
        case 0x87: return 0x2021;
        case 0x88: return 0x02c6;
        case 0x89: return 0x2030;
        case 0x8a: return 0x0160;
        case 0x8b: return 0x2039;
        case 0x8c: return 0x0152;
        case 0x8e: return 0x017d;
        case 0x91: return 0x2018;
        case 0x92: return 0x2019;
        case 0x93: return 0x201c;
        case 0x94: return 0x201d;
        case 0x95: return 0x2022;
        case 0x96: return 0x2013;
        case 0x97: return 0x2014;
        case 0x98: return 0x02dc;
        case 0x99: return 0x2122;
        case 0x9a: return 0x0161;
        case 0x9b: return 0x203a;
        case 0x9c: return 0x0153;
        case 0x9e: return 0x017e;
        case 0x9f: return 0x0178;
        default:   return code;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Byte classes
//
// The meaning of an input byte for the states that scan runs of bytes. A scan leaves its loop at the bytes that its
// state has a rule for, and at bytes whose code point html/#preprocessing-the-input-stream may report. It accumulates
// the class of every other byte.

enum {
    ByteClass_Text = 1 << 0,                                    // neither ASCII whitespace nor a null
    ByteClass_Space = 1 << 1,                                   // tab, line feed, form feed, carriage return, space
    ByteClass_Null = 1 << 2,
    ByteClass_Lt = 1 << 3,                                      // '<'
    ByteClass_Amp = 1 << 4,                                     // '&'
    ByteClass_Gt = 1 << 5,                                      // '>'
    ByteClass_Dash = 1 << 6,                                    // '-'
    ByteClass_Bracket = 1 << 7,                                 // ']'
    ByteClass_Quote = 1 << 8,                                   // '"' or '\''
    ByteClass_Backtick = 1 << 9,                                // '`'
    ByteClass_Equals = 1 << 10,                                 // '='
    ByteClass_Solidus = 1 << 11,                                // '/'
    ByteClass_Upper = 1 << 12,                                  // an ASCII uppercase letter
    ByteClass_Alnum = 1 << 13,                                  // an ASCII letter or digit

    // Starts a sequence that html/#preprocessing-the-input-stream may report.
    ByteClass_Look = 1 << 14,

    ByteClass_NameEnd = ByteClass_Space | ByteClass_Gt,         // ends the name of a doctype
    ByteClass_TagEnd = ByteClass_NameEnd | ByteClass_Solidus,   // ends the name of a tag or an attribute
};
typedef uint16_t ByteClass;

struct HtmlByteTable {
    ByteClass of[256];
};

// Returns true if any code point from `lo` to `hi` is one that html/#preprocessing-the-input-stream reports. The C1
// controls are the only code points above ASCII for which isControl returns true.
constexpr bool anyReported(uint32_t lo, uint32_t hi) {
    bool planeEnd = (hi & 0xffff) >= 0xfffe || (hi >> 16) > (lo >> 16);
    return planeEnd || (lo <= 0xdfff && hi >= 0xd800) || (lo <= 0xfdef && hi >= 0xfdd0) || (lo <= 0x9f && hi >= 0x80);
}

// Returns true if a byte starts a UTF-8 sequence that encodes such a code point. A continuation byte starts no
// sequence. The remaining lead bytes cannot reach a surrogate, a noncharacter or a control.
constexpr bool opensReported(uint32_t byte) {
    if (byte < 0x80)
        return isControl(byte);
    if (byte < 0xc2)
        return false;
    if (byte < 0xe0)
        return anyReported((byte & 0x1f) << 6, ((byte & 0x1f) << 6) | 0x3f);
    if (byte < 0xf0)
        return anyReported((byte & 0x0f) << 12, ((byte & 0x0f) << 12) | 0xfff);
    if (byte < 0xf8)
        return anyReported((byte & 0x07) << 18, ((byte & 0x07) << 18) | 0x3ffff);
    return false;
}

constexpr HtmlByteTable makeHtmlBytes() {
    HtmlByteTable table = {};
    for (uint32_t b = 0; b < 256; ++b) {
        char c = char(b);
        bool space = asciiIsSpace(c);

        ByteClass bits = 0;
        if (b == 0)
            bits |= ByteClass_Null;
        else if (space)
            bits |= ByteClass_Space;
        else
            bits |= ByteClass_Text;

        if (b == '<')
            bits |= ByteClass_Lt;
        if (b == '&')
            bits |= ByteClass_Amp;
        if (b == '>')
            bits |= ByteClass_Gt;
        if (b == '-')
            bits |= ByteClass_Dash;
        if (b == ']')
            bits |= ByteClass_Bracket;
        if (b == '"' || b == '\'')
            bits |= ByteClass_Quote;
        if (b == '`')
            bits |= ByteClass_Backtick;
        if (b == '=')
            bits |= ByteClass_Equals;
        if (b == '/')
            bits |= ByteClass_Solidus;
        if (b < 0x80 && asciiIsUpper(c))
            bits |= ByteClass_Upper;
        if (b < 0x80 && (asciiIsLetter(c) || asciiIsDigit(c)))
            bits |= ByteClass_Alnum;
        if (opensReported(b))
            bits |= ByteClass_Look;
        table.of[b] = bits;
    }
    return table;
}

constexpr HtmlByteTable kHtmlBytes = makeHtmlBytes();

ByteClass classOf(char byte) {
    return kHtmlBytes.of[uint8_t(byte)];
}

// Returns the class of a byte read as a character, from the byte class lookup. The end of the input has no class.
ByteClass classOf(uint32_t byte) {
    return byte < 0x100 ? kHtmlBytes.of[byte] : 0;
}

// Returns the flags of a character token for characters whose classes `seen` accumulates.
HtmlTokenFlags textFlags(ByteClass seen) {
    HtmlTokenFlags flags = 0;
    if ((seen & ByteClass_Space) != 0)
        flags |= HtmlTokenFlag_Whitespace;
    if ((seen & ByteClass_Text) != 0)
        flags |= HtmlTokenFlag_Text;
    if ((seen & ByteClass_Null) != 0)
        flags |= HtmlTokenFlag_Null;
    return flags;
}

// Returns the classes of every byte of `text`. A run of text that is not the input's own bytes gets its token's flags
// this way.
ByteClass classesOf(PrStringView text) {
    ByteClass seen = 0;
    for (size_t i = 0; i < text.size; ++i)
        seen |= classOf(text.data[i]);
    return seen;
}

// ---------------------------------------------------------------------------------------------------------------------
// Reading

void errorAt(HtmlTokenizer* t, HtmlErrorCode code, uint32_t offset) {
    t->errors->push(HtmlError{code, offset});
    if (code >= HtmlErrorCode_EofBeforeTagName && code <= HtmlErrorCode_EofInTag)
        t->cut = true;
}

// Reports the code point errors that html/#preprocessing-the-input-stream names for the sequence at `offset`, and
// returns its width. A byte is reported once, however many times a state reads it.
uint32_t check(HtmlTokenizer* t, uint32_t offset) {
    uint32_t codePoint = 0;
    uint32_t width = prUtf8Decode(htmlInputText(t), offset, &codePoint);
    if (offset < t->checked)
        return width;

    t->checked = offset + width;
    if (isSurrogate(codePoint))
        errorAt(t, HtmlErrorCode_SurrogateInInputStream, offset);
    else if (isNoncharacter(codePoint))
        errorAt(t, HtmlErrorCode_NoncharacterInInputStream, offset);
    else if (isControl(codePoint))
        errorAt(t, HtmlErrorCode_ControlCharacterInInputStream, offset);
    return width;
}

// Returns the byte at `offset`, or kEof at the end of the input. Reading a character reports the code point errors that
// html/#preprocessing-the-input-stream names for it, so that a state has them before it decides what the character is.
uint32_t readByte(HtmlTokenizer* t, uint32_t offset) {
    if (offset >= t->input.text.count)
        return kEof;

    uint32_t byte = uint8_t(t->input.text.data[offset]);
    if ((classOf(byte) & ByteClass_Look) != 0)
        check(t, offset);
    return byte;
}

// Moves the read point past any ASCII whitespace there, and returns true if there was any.
bool skipSpace(HtmlTokenizer* t) {
    PrStringView text = htmlInputText(t);
    uint32_t at = t->input.pos;
    uint32_t from = at;
    while (at < text.size && (classOf(text.data[at]) & ByteClass_Space) != 0)
        ++at;
    t->input.pos = at;
    return at != from;
}

// ---------------------------------------------------------------------------------------------------------------------
// Building text

HtmlSpan begin(uint32_t offset) {
    return HtmlSpan{offset, offset};
}

// Copies the text of `span` into the values buffer, which is where text that is not the input's own bytes is built.
void moveToValues(HtmlTokenizer* t, HtmlSpan* span) {
    uint32_t start = t->values.count;
    t->values.append(t->input.text.data + span->start, span->end - span->start);
    span->start = start | kHtmlSpanValues;
}

// Extends `span` with text that is not the input's own bytes.
void appendText(HtmlTokenizer* t, HtmlSpan* span, PrStringView text) {
    if ((span->start & kHtmlSpanValues) == 0)
        moveToValues(t, span);
    t->values.append(text.data, uint32_t(text.size));
    span->end = t->values.count;
}

void appendInput(HtmlTokenizer* t, HtmlSpan* span, uint32_t from, uint32_t to) {
    if (from == to)
        return;
    if ((span->start & kHtmlSpanValues) == 0 && span->end == from) {
        span->end = to;
        return;
    }
    appendText(t, span, PrStringView{t->input.text.data + from, to - from});
}

void appendChar(HtmlTokenizer* t, HtmlSpan* span, char value) {
    if ((span->start & kHtmlSpanValues) == 0)
        moveToValues(t, span);
    t->values.push(value);
    span->end = t->values.count;
}

// Scans from the read point to the first byte with one of the classes in `stop`. Adds the bytes it passes to `span`,
// and reports the code point errors that html/#preprocessing-the-input-stream names for them. Returns the byte that
// stopped it, or kEof at the end of the input.
uint32_t take(HtmlTokenizer* t, HtmlSpan* span, ByteClass stop) {
    PrStringView text = htmlInputText(t);
    ByteClass leave = stop | ByteClass_Look;
    uint32_t at = t->input.pos;
    for (;;) {
        uint32_t from = at;
        while (at < text.size && (classOf(text.data[at]) & leave) == 0)
            ++at;
        appendInput(t, span, from, at);
        t->input.pos = at;
        if (at >= text.size)
            return kEof;

        uint32_t byte = uint32_t(uint8_t(text.data[at]));
        if ((classOf(byte) & stop) != 0)
            return byte;

        uint32_t width = check(t, at);
        appendInput(t, span, at, at + width);
        at += width;
        t->input.pos = at;
    }
}

// Scans like take, but over characters from which no token is built. Stops at the first byte with one of the classes in
// `stop`, and returns the classes of the bytes that it passed.
ByteClass scan(HtmlTokenizer* t, ByteClass stop) {
    PrStringView text = htmlInputText(t);
    ByteClass leave = stop | ByteClass_Look;
    ByteClass seen = 0;
    uint32_t at = t->input.pos;
    while (at < text.size) {
        ByteClass bits = classOf(text.data[at]);
        if ((bits & leave) == 0) {
            seen |= bits;
            ++at;
            continue;
        }
        if ((bits & stop) != 0)
            break;
        seen |= bits;
        at += check(t, at);
    }
    t->input.pos = at;
    return seen;
}

// ---------------------------------------------------------------------------------------------------------------------
// Emitting

bool emitEof(HtmlTokenizer* t) {
    t->token.kind = HtmlTokenKind_Eof;
    t->token.span = HtmlSpan{t->input.pos, t->input.pos};
    t->state = HtmlState_Data;
    return true;
}

// Emits the characters in `chars`. `source` is the input that they were written with.
bool emitCharacters(HtmlTokenizer* t, HtmlSpan chars, HtmlSpan source, HtmlTokenFlags flags, HtmlState next) {
    t->token.kind = HtmlTokenKind_Character;
    t->token.flags = flags;
    t->token.span = source;
    t->token.data = chars;
    t->state = next;
    return true;
}

// Emits a run of the input's own bytes, whose classes `seen` accumulates.
bool emitRun(HtmlTokenizer* t, uint32_t from, uint32_t to, ByteClass seen, HtmlState next) {
    return emitCharacters(t, HtmlSpan{from, to}, HtmlSpan{from, to}, textFlags(seen), next);
}

// Emits the run of nulls at the read point. html/#data-state emits them as characters and reports each one.
// html/#cdata-section-state emits them without reporting any.
bool emitNulls(HtmlTokenizer* t, bool report, HtmlState next) {
    PrStringView text = htmlInputText(t);
    uint32_t start = t->input.pos;
    uint32_t at = start;
    while (at < text.size && text.data[at] == '\0') {
        if (report)
            errorAt(t, HtmlErrorCode_UnexpectedNullCharacter, at);
        ++at;
    }
    t->input.pos = at;
    return emitCharacters(t, HtmlSpan{start, at}, HtmlSpan{start, at}, HtmlTokenFlag_Null, next);
}

// Emits the same run with U+FFFD for each null, as html/#rcdata-state through html/#plaintext-state specify.
bool emitReplacement(HtmlTokenizer* t, HtmlState next) {
    PrStringView text = htmlInputText(t);
    uint32_t start = t->input.pos;
    uint32_t values = t->values.count;
    uint32_t at = start;
    while (at < text.size && text.data[at] == '\0') {
        errorAt(t, HtmlErrorCode_UnexpectedNullCharacter, at);
        t->values.append(kReplacement.data, uint32_t(kReplacement.size));
        ++at;
    }
    t->input.pos = at;
    return emitCharacters(t, HtmlSpan{values | kHtmlSpanValues, t->values.count},
        HtmlSpan{start, at}, HtmlTokenFlag_Text, next);
}

// Emits a tag. The read point is past its '>', and its '<' is at `start`.
bool emitTag(HtmlTokenizer* t, uint32_t start, HtmlSpan name) {
    if (t->token.kind == HtmlTokenKind_EndTag) {
        if (t->attributes.count != 0)
            errorAt(t, HtmlErrorCode_EndTagWithAttributes, t->input.pos - 1);
        if ((t->token.flags & HtmlTokenFlag_SelfClosing) != 0)
            errorAt(t, HtmlErrorCode_EndTagWithTrailingSolidus, t->input.pos - 1);
        t->attributes.count = 0;
    }
    t->token.name = name;
    t->token.span = HtmlSpan{start, t->input.pos};
    t->state = HtmlState_Data;
    return true;
}

bool emitComment(HtmlTokenizer* t, uint32_t start, HtmlSpan data) {
    t->token.kind = HtmlTokenKind_Comment;
    t->token.data = data;
    t->token.span = HtmlSpan{start, t->input.pos};
    t->state = HtmlState_Data;
    return true;
}

bool emitDoctype(HtmlTokenizer* t, uint32_t start, HtmlSpan name, HtmlSpan publicId, HtmlSpan systemId) {
    t->token.kind = HtmlTokenKind_Doctype;
    t->token.name = name;
    t->token.publicId = publicId;
    t->token.systemId = systemId;
    t->token.span = HtmlSpan{start, t->input.pos};
    t->state = HtmlState_Data;
    return true;
}

bool emitProcessingInstruction(HtmlTokenizer* t, uint32_t start, HtmlSpan name, HtmlSpan data) {
    t->token.kind = HtmlTokenKind_ProcessingInstruction;
    t->token.name = name;
    t->token.data = data;
    t->token.span = HtmlSpan{start, t->input.pos};
    t->state = HtmlState_Data;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Character references (html/#character-reference-state to html/#numeric-character-reference-end-state)

// Returns the number of bytes of the longest named character reference in the input at `from`, matched as
// html/#named-character-reference-state consumes one. Writes the text that it expands to into `text`.
//
// A name consists of ASCII alphanumerics and at most one trailing semicolon. So the whole run is the only candidate
// that a semicolon can end, and every shorter candidate is a name written without its semicolon. That bounds the walk
// at kPrHtmlEntityMaxLegacyLength rather than at the longest name.
uint32_t matchEntity(HtmlTokenizer* t, uint32_t from, PrStringView* text) {
    PrStringView source = htmlInputText(t);
    uint32_t end = from;
    while (end < source.size && end - from < kPrHtmlEntityMaxLength && (classOf(source.data[end]) & ByteClass_Alnum) != 0)
        ++end;
    if (end == source.size && end - from < kPrHtmlEntityMaxLength)
        t->cut = true;

    if (end < source.size && end - from < kPrHtmlEntityMaxLength && source.data[end] == ';') {
        PrStringView match = prHtmlEntityMatch(stringSlice(source, from, end + 1 - from));
        if (match.data != nullptr) {
            *text = match;
            return end + 1 - from;
        }
    }

    uint32_t length = end - from;
    if (length > kPrHtmlEntityMaxLegacyLength)
        length = kPrHtmlEntityMaxLegacyLength;
    for (; length != 0; --length) {
        PrStringView match = prHtmlEntityMatch(stringSlice(source, from, length));
        if (match.data != nullptr) {
            *text = match;
            return length;
        }
    }
    return 0;
}

// Flushes the input's own bytes from `from` to `to`, which are the characters of a reference that resolved to nothing.
// `value` is the attribute value that they join. In text, `value` is null and the bytes become a character token.
void flushInput(HtmlTokenizer* t, HtmlSpan* PR_NULLABLE value, uint32_t from, uint32_t to, HtmlState next) {
    if (value != nullptr) {
        appendInput(t, value, from, to);
        return;
    }
    PrStringView text = htmlInputText(t);
    emitRun(t, from, to, classesOf(stringSlice(text, from, to - from)), next);
}

// Flushes the text that a reference resolved to, which is not the input's own bytes, in the same way as flushInput.
void flushText(HtmlTokenizer* t, HtmlSpan* PR_NULLABLE value, PrStringView resolved,
        uint32_t from, uint32_t to, HtmlState next) {
    if (value != nullptr) {
        appendText(t, value, resolved);
        return;
    }
    uint32_t at = t->values.count;
    t->values.append(resolved.data, uint32_t(resolved.size));
    emitCharacters(t, HtmlSpan{at | kHtmlSpanValues, t->values.count}, HtmlSpan{from, to},
        textFlags(classesOf(resolved)), next);
}

// Reads the character reference whose '&' is at the read point. If the reference is part of an attribute value, `value`
// receives the text. In text, `value` is null and the text becomes a character token read in state `next`.
void readReference(HtmlTokenizer* t, HtmlSpan* PR_NULLABLE value, HtmlState next) {
    PrStringView text = htmlInputText(t);
    uint32_t start = t->input.pos;
    uint32_t at = start + 1;
    uint32_t byte = readByte(t, at);

    // html/#numeric-character-reference-state to html/#numeric-character-reference-end-state: a reference written as a
    // number.
    if (byte == '#') {
        uint32_t digits = at + 1;
        uint32_t base = 10;
        uint32_t mark = readByte(t, digits);
        if (mark == 'x' || mark == 'X') {
            base = 16;
            ++digits;
        }

        uint32_t code = 0;
        uint32_t end = digits;
        while (end < text.size && (base == 16 ? asciiIsHexDigit(text.data[end]) : asciiIsDigit(text.data[end]))) {
            if (code <= kPrUtf8MaxCodePoint)
                code = code * base + asciiHexValue(text.data[end]);
            ++end;
        }
        if (end == digits) {
            errorAt(t, HtmlErrorCode_AbsenceOfDigitsInNumericCharacterReference, end);
            t->input.pos = end;
            flushInput(t, value, start, end, next);
            return;
        }
        if (end < text.size && text.data[end] == ';')
            ++end;
        else
            errorAt(t, HtmlErrorCode_MissingSemicolonAfterCharacterReference, end);
        t->input.pos = end;

        // html/#numeric-character-reference-end-state: the code points that a reference may not name.
        if (code == 0) {
            errorAt(t, HtmlErrorCode_NullCharacterReference, end);
            code = kPrUtf8Replacement;
        }
        if (code > kPrUtf8MaxCodePoint) {
            errorAt(t, HtmlErrorCode_CharacterReferenceOutsideUnicodeRange, end);
            code = kPrUtf8Replacement;
        }
        if (isSurrogate(code)) {
            errorAt(t, HtmlErrorCode_SurrogateCharacterReference, end);
            code = kPrUtf8Replacement;
        }
        if (isNoncharacter(code))
            errorAt(t, HtmlErrorCode_NoncharacterCharacterReference, end);
        if (code == '\r' || isControl(code)) {
            errorAt(t, HtmlErrorCode_ControlCharacterReference, end);
            code = numericReplacement(code);
        }

        char encoded[kPrUtf8MaxBytes];
        uint32_t width = prUtf8Encode(code, encoded);
        flushText(t, value, PrStringView{encoded, width}, start, end, next);
        return;
    }

    // html/#named-character-reference-state: a reference written as a name.
    if ((classOf(byte) & ByteClass_Alnum) != 0) {
        PrStringView resolved = {nullptr, 0};
        uint32_t length = matchEntity(t, at, &resolved);
        if (length == 0) {
            // html/#ambiguous-ampersand-state. The flush already covers its characters.
            uint32_t end = at;
            while (end < text.size && (classOf(text.data[end]) & ByteClass_Alnum) != 0)
                ++end;
            if (end < text.size && text.data[end] == ';')
                errorAt(t, HtmlErrorCode_UnknownNamedCharacterReference, end);
            t->input.pos = end;
            flushInput(t, value, start, end, next);
            return;
        }

        uint32_t end = at + length;
        bool semicolon = text.data[end - 1] == ';';
        if (value != nullptr && !semicolon && end < text.size &&
                (text.data[end] == '=' || (classOf(text.data[end]) & ByteClass_Alnum) != 0)) {
            t->input.pos = end;
            flushInput(t, value, start, end, next);
            return;
        }
        if (!semicolon)
            errorAt(t, HtmlErrorCode_MissingSemicolonAfterCharacterReference, end);
        t->input.pos = end;
        flushText(t, value, resolved, start, end, next);
        return;
    }

    // html/#character-reference-state: a '&' that starts nothing stands for itself.
    t->input.pos = at;
    flushInput(t, value, start, at, next);
}

// ---------------------------------------------------------------------------------------------------------------------
// Tags and attributes (html/#tag-open-state to html/#self-closing-start-tag-state)

// Indicates no attribute. The current attribute becomes this value after a duplicate name drops it.
constexpr uint32_t kNoAttribute = 0xffffffffu;

// Reads a name, lowercased, into `name` from the read point. Returns the character that ended it, and leaves the read
// point on that character, so that no byte of the name is read twice.
uint32_t readName(HtmlTokenizer* t, HtmlSpan* name, ByteClass stop) {
    for (;;) {
        uint32_t byte = take(t, name, stop | ByteClass_Upper | ByteClass_Null);
        if (byte == kEof || (classOf(byte) & stop) != 0)
            return byte;
        if (byte == 0) {
            errorAt(t, HtmlErrorCode_UnexpectedNullCharacter, t->input.pos);
            appendText(t, name, kReplacement);
        } else {
            appendChar(t, name, asciiLower(char(byte)));
        }
        ++t->input.pos;
    }
}

// Returns true if the '<' at `offset` starts the end tag of the element whose content the state was entered for
// (html/#tokenization). Its name consists of ASCII letters, which are all that the end tag name states accept.
bool isTextEndTag(const HtmlTokenizer* t, uint32_t offset) {
    if (prStringIsNull(&t->lastStartTag))
        return false;

    PrStringView text = htmlInputText(t);
    PrStringView tag = prStringView(&t->lastStartTag);
    uint32_t from = offset + 2;
    if (offset + 1 >= text.size || text.data[offset + 1] != '/' || from + tag.size >= text.size)
        return false;

    for (size_t i = 0; i < tag.size; ++i) {
        char byte = text.data[from + i];
        if (!asciiIsLetter(byte) || asciiLower(byte) != asciiLower(tag.data[i]))
            return false;
    }
    return (classOf(text.data[from + tag.size]) & ByteClass_TagEnd) != 0;
}

// Adds the attribute to the token, unless it already has one with that name (html/#attribute-name-state).
uint32_t pushAttribute(HtmlTokenizer* t, HtmlSpan name, HtmlSpan value) {
    PrStringView text = htmlSpanText(t, name);
    for (uint32_t i = 0; i < t->attributes.count; ++i) {
        if (htmlSpanText(t, t->attributes.data[i].name) == text) {
            errorAt(t, HtmlErrorCode_DuplicateAttribute, t->input.pos);
            return kNoAttribute;
        }
    }
    t->attributes.push(HtmlAttribute{name, value});
    return t->attributes.count - 1;
}

// html/#attribute-value-(double-quoted)-state to html/#attribute-value-(unquoted)-state: reads an attribute's value.
// `quote` is the quote that the value started with, or kEof if it started with none. Returns the character that follows
// the value, or kEof if the end of the input ended the tag.
uint32_t readAttributeValue(HtmlTokenizer* t, uint32_t quote, HtmlSpan* value) {
    ByteClass stop;
    if (quote != kEof)
        stop = ByteClass(ByteClass_Quote | ByteClass_Amp | ByteClass_Null);
    else
        stop = ByteClass(ByteClass_Space | ByteClass_Gt | ByteClass_Amp | ByteClass_Null | ByteClass_Quote |
            ByteClass_Backtick | ByteClass_Lt | ByteClass_Equals);
    for (;;) {
        uint32_t byte = take(t, value, stop);
        if (byte == kEof)
            return kEof;
        if (byte == quote) {
            ++t->input.pos;
            return readByte(t, t->input.pos);
        }
        if (quote == kEof && (classOf(byte) & (ByteClass_Space | ByteClass_Gt)) != 0)
            return byte;
        if (byte == '&') {
            readReference(t, value, HtmlState_Data);
            continue;
        }
        if (byte == 0) {
            errorAt(t, HtmlErrorCode_UnexpectedNullCharacter, t->input.pos);
            appendText(t, value, kReplacement);
            ++t->input.pos;
            continue;
        }
        // html/#attribute-value-(unquoted)-state: a quote, a backtick, a '<' or an '=' in a value written without
        // quotes.
        if (quote == kEof)
            errorAt(t, HtmlErrorCode_UnexpectedCharacterInUnquotedAttributeValue, t->input.pos);
        appendInput(t, value, t->input.pos, t->input.pos + 1);
        ++t->input.pos;
    }
}

// html/#before-attribute-name-state to html/#after-attribute-value-(quoted)-state: reads one attribute of the current
// tag, starting at `byte`. Returns the character that ended it, or kEof if the end of the input ended the tag.
uint32_t readAttribute(HtmlTokenizer* t, uint32_t byte) {
    constexpr ByteClass kAttributeEnd = ByteClass_TagEnd | ByteClass_Equals | ByteClass_Quote | ByteClass_Lt;

    uint32_t start = t->input.pos;
    HtmlSpan name = begin(start);
    if (byte == '=') {
        // html/#before-attribute-name-state treats an '=' at the start of a name as its first character.
        errorAt(t, HtmlErrorCode_UnexpectedEqualsSignBeforeAttributeName, start);
        ++t->input.pos;
        appendInput(t, &name, start, start + 1);
    }

    byte = readName(t, &name, kAttributeEnd);
    while (byte != kEof && (classOf(byte) & (ByteClass_TagEnd | ByteClass_Equals)) == 0) {
        // html/#attribute-name-state: a quote or a '<' is a name character that is never written on purpose.
        errorAt(t, HtmlErrorCode_UnexpectedCharacterInAttributeName, t->input.pos);
        appendInput(t, &name, t->input.pos, t->input.pos + 1);
        ++t->input.pos;
        byte = readName(t, &name, kAttributeEnd);
    }

    HtmlSpan value = begin(t->input.pos);
    uint32_t attr = pushAttribute(t, name, value);
    if ((classOf(byte) & ByteClass_Space) != 0) {
        skipSpace(t);
        byte = readByte(t, t->input.pos);
    }
    if (byte != '=')
        return byte;

    ++t->input.pos;
    skipSpace(t);
    byte = readByte(t, t->input.pos);
    if (byte == '>') {
        errorAt(t, HtmlErrorCode_MissingAttributeValue, t->input.pos);
        return byte;
    }

    uint32_t quote = kEof;
    if ((classOf(byte) & ByteClass_Quote) != 0) {
        quote = byte;
        ++t->input.pos;
    }
    value = begin(t->input.pos);
    byte = readAttributeValue(t, quote, &value);
    if (attr != kNoAttribute)
        t->attributes.data[attr].value = value;

    // html/#after-attribute-value-(quoted)-state: what follows a quoted value starts a new attribute only if
    // whitespace, a '/' or a '>' separates the two.
    if (quote != kEof && byte != kEof && (classOf(byte) & ByteClass_TagEnd) == 0)
        errorAt(t, HtmlErrorCode_MissingWhitespaceBetweenAttributes, t->input.pos);
    return byte;
}

// html/#tag-name-state to html/#self-closing-start-tag-state: reads a tag's name, its attributes and the '>' that ends
// it. The read point is the first byte of the name, and `start` is the '<' that the tag was written with.
bool readTag(HtmlTokenizer* t, HtmlTokenKind kind, uint32_t start) {
    t->token.kind = kind;
    t->attributes.count = 0;

    HtmlSpan name = begin(t->input.pos);
    uint32_t byte = readName(t, &name, ByteClass_TagEnd);
    for (;;) {
        if (byte == '>') {
            ++t->input.pos;
            return emitTag(t, start, name);
        }
        // Every way a tag can reach the end of the input arrives here, on the byte past the end.
        if (byte == kEof) {
            errorAt(t, HtmlErrorCode_EofInTag, t->input.pos);
            return emitEof(t);
        }
        if ((classOf(byte) & ByteClass_Space) != 0) {
            skipSpace(t);
            byte = readByte(t, t->input.pos);
            continue;
        }
        if (byte == '/') {
            // html/#self-closing-start-tag-state: the solidus of a self-closing tag.
            ++t->input.pos;
            byte = readByte(t, t->input.pos);
            if (byte == '>') {
                ++t->input.pos;
                t->token.flags |= HtmlTokenFlag_SelfClosing;
                return emitTag(t, start, name);
            }
            if (byte != kEof)
                errorAt(t, HtmlErrorCode_UnexpectedSolidusInTag, t->input.pos);
            continue;
        }
        byte = readAttribute(t, byte);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Comments (html/#bogus-comment-state to html/#comment-end-bang-state)

// Reads a comment whose text is everything up to the next '>'.
bool readBogusComment(HtmlTokenizer* t, uint32_t start, HtmlSpan data) {
    for (;;) {
        uint32_t byte = take(t, &data, ByteClass_Gt | ByteClass_Null);
        if (byte == kEof)
            return emitComment(t, start, data);
        if (byte == '>') {
            ++t->input.pos;
            return emitComment(t, start, data);
        }
        errorAt(t, HtmlErrorCode_UnexpectedNullCharacter, t->input.pos);
        appendText(t, &data, kReplacement);
        ++t->input.pos;
    }
}

// html/#comment-end-dash-state and html/#comment-end-state: reads what follows the two dashes that may end a comment.
// The read point is past them. Returns true if the comment was emitted.
bool readCommentEnd(HtmlTokenizer* t, uint32_t start, HtmlSpan* data) {
    for (;;) {
        uint32_t byte = readByte(t, t->input.pos);
        if (byte == '>') {
            ++t->input.pos;
            emitComment(t, start, *data);
            return true;
        }
        if (byte == kEof) {
            errorAt(t, HtmlErrorCode_EofInComment, t->input.pos);
            emitComment(t, start, *data);
            return true;
        }
        if (byte == '-') {
            appendText(t, data, "-");
            ++t->input.pos;
            continue;
        }
        if (byte != '!') {
            appendText(t, data, "--");
            return false;
        }

        // html/#comment-end-bang-state: the "--!" of a comment closed like a markup declaration.
        ++t->input.pos;
        uint32_t after = readByte(t, t->input.pos);
        if (after == '>') {
            errorAt(t, HtmlErrorCode_IncorrectlyClosedComment, t->input.pos);
            ++t->input.pos;
            emitComment(t, start, *data);
            return true;
        }
        if (after == kEof) {
            errorAt(t, HtmlErrorCode_EofInComment, t->input.pos);
            emitComment(t, start, *data);
            return true;
        }
        appendText(t, data, "--!");
        return false;
    }
}

// Reads a comment. The read point is past its "<!--", and its '<' is at `start`.
bool readComment(HtmlTokenizer* t, uint32_t start) {
    uint32_t from = t->input.pos;
    HtmlSpan data = begin(from);

    // html/#comment-start-state and html/#comment-start-dash-state: the comment is empty if a '>' follows the "<!--",
    // with or without a '-' before it, or if the input ends after a '-'.
    uint32_t dash = readByte(t, from) == '-' ? 1 : 0;
    uint32_t after = readByte(t, from + dash);
    if (after == '>') {
        errorAt(t, HtmlErrorCode_AbruptClosingOfEmptyComment, from + dash);
        t->input.pos = from + dash + 1;
        return emitComment(t, start, data);
    }
    if (dash != 0 && after == kEof) {
        errorAt(t, HtmlErrorCode_EofInComment, from + dash);
        t->input.pos = from + dash;
        return emitComment(t, start, data);
    }

    for (;;) {
        uint32_t byte = take(t, &data, ByteClass_Dash | ByteClass_Lt | ByteClass_Null);
        if (byte == kEof) {
            errorAt(t, HtmlErrorCode_EofInComment, t->input.pos);
            return emitComment(t, start, data);
        }
        if (byte == 0) {
            errorAt(t, HtmlErrorCode_UnexpectedNullCharacter, t->input.pos);
            appendText(t, &data, kReplacement);
            ++t->input.pos;
            continue;
        }

        if (byte == '<') {
            // html/#comment-less-than-sign-state to html/#comment-less-than-sign-bang-dash-dash-state: a "<!--" written
            // inside a comment. Its dashes are the two that may end the comment, and the nested comment is reported.
            appendInput(t, &data, t->input.pos, t->input.pos + 1);
            ++t->input.pos;
            if (readByte(t, t->input.pos) != '!')
                continue;
            appendInput(t, &data, t->input.pos, t->input.pos + 1);
            ++t->input.pos;
            if (readByte(t, t->input.pos) != '-')
                continue;
            ++t->input.pos;
            if (readByte(t, t->input.pos) != '-') {
                appendText(t, &data, "-");
                continue;
            }
            ++t->input.pos;
            uint32_t nested = readByte(t, t->input.pos);
            if (nested != '>' && nested != kEof)
                errorAt(t, HtmlErrorCode_NestedComment, t->input.pos);
            if (readCommentEnd(t, start, &data))
                return true;
            continue;
        }

        // html/#comment-end-dash-state: a dash, which ends the comment if another follows it.
        ++t->input.pos;
        if (readByte(t, t->input.pos) != '-') {
            if (readByte(t, t->input.pos) == kEof) {
                errorAt(t, HtmlErrorCode_EofInComment, t->input.pos);
                return emitComment(t, start, data);
            }
            appendText(t, &data, "-");
            continue;
        }
        ++t->input.pos;
        if (readCommentEnd(t, start, &data))
            return true;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Doctypes (html/#doctype-state to html/#bogus-doctype-state)

// The errors that html/#parse-errors names for one of a doctype's two identifiers. The two sets differ only in which
// identifier they name.
struct DoctypeIdentifier {
    HtmlErrorCode missingSpace;   // no whitespace between the keyword and the identifier
    HtmlErrorCode missing;        // a '>' where the identifier should be
    HtmlErrorCode missingQuote;   // an identifier written without quotes
    HtmlErrorCode abrupt;         // a '>' inside the identifier
    HtmlTokenFlags absent;        // the flag that the identifier's opening quote clears
};

constexpr DoctypeIdentifier kDoctypePublic = {
    HtmlErrorCode_MissingWhitespaceAfterDoctypePublicKeyword,
    HtmlErrorCode_MissingDoctypePublicIdentifier,
    HtmlErrorCode_MissingQuoteBeforeDoctypePublicIdentifier,
    HtmlErrorCode_AbruptDoctypePublicIdentifier,
    HtmlTokenFlag_NoPublicId,
};

constexpr DoctypeIdentifier kDoctypeSystem = {
    HtmlErrorCode_MissingWhitespaceAfterDoctypeSystemKeyword,
    HtmlErrorCode_MissingDoctypeSystemIdentifier,
    HtmlErrorCode_MissingQuoteBeforeDoctypeSystemIdentifier,
    HtmlErrorCode_AbruptDoctypeSystemIdentifier,
    HtmlTokenFlag_NoSystemId,
};

// html/#bogus-doctype-state: skips the rest of a doctype from which nothing can be read, up to the next '>'.
void skipBogusDoctype(HtmlTokenizer* t) {
    for (;;) {
        scan(t, ByteClass_Gt | ByteClass_Null);
        uint32_t at = t->input.pos;
        uint32_t byte = readByte(t, at);
        if (byte == kEof)
            return;
        t->input.pos = at + 1;
        if (byte == '>')
            return;
        errorAt(t, HtmlErrorCode_UnexpectedNullCharacter, at);
    }
}

// html/#before-doctype-public-identifier-state, html/#doctype-public-identifier-(double-quoted)-state,
// html/#before-doctype-system-identifier-state and html/#doctype-system-identifier-(double-quoted)-state: reads the
// text of an identifier. The read point is past its opening `quote`. Returns false if a '>' or the end of the input
// ended the doctype instead of the quote.
bool readIdentifierText(HtmlTokenizer* t, uint32_t quote, HtmlSpan* into, HtmlErrorCode abrupt) {
    for (;;) {
        uint32_t byte = take(t, into, ByteClass_Quote | ByteClass_Gt | ByteClass_Null);
        if (byte == kEof) {
            errorAt(t, HtmlErrorCode_EofInDoctype, t->input.pos);
            t->token.flags |= HtmlTokenFlag_ForceQuirks;
            return false;
        }
        if (byte == quote) {
            ++t->input.pos;
            return true;
        }
        if (byte == '>') {
            errorAt(t, abrupt, t->input.pos);
            t->token.flags |= HtmlTokenFlag_ForceQuirks;
            ++t->input.pos;
            return false;
        }
        if (byte == 0) {
            errorAt(t, HtmlErrorCode_UnexpectedNullCharacter, t->input.pos);
            appendText(t, into, kReplacement);
            ++t->input.pos;
            continue;
        }
        appendInput(t, into, t->input.pos, t->input.pos + 1);
        ++t->input.pos;
    }
}

// html/#after-doctype-name-state to html/#doctype-public-identifier-(double-quoted)-state and
// html/#between-doctype-public-and-system-identifiers-state to html/#doctype-system-identifier-(double-quoted)-state:
// reads the identifier that a doctype's PUBLIC or SYSTEM keyword introduces. The read point is past the keyword.
// Returns false if the doctype ended without an identifier, after recording its errors and its force-quirks flag.
bool readIdentifier(HtmlTokenizer* t, const DoctypeIdentifier& codes, HtmlSpan* into) {
    bool spaced = skipSpace(t);
    uint32_t at = t->input.pos;
    uint32_t byte = readByte(t, at);
    if ((classOf(byte) & ByteClass_Quote) != 0) {
        if (!spaced)
            errorAt(t, codes.missingSpace, at);
        t->input.pos = at + 1;
        t->token.flags &= ~codes.absent;
        *into = begin(t->input.pos);
        return readIdentifierText(t, byte, into, codes.abrupt);
    }

    t->token.flags |= HtmlTokenFlag_ForceQuirks;
    if (byte == '>') {
        errorAt(t, codes.missing, at);
        t->input.pos = at + 1;
        return false;
    }
    if (byte == kEof) {
        errorAt(t, HtmlErrorCode_EofInDoctype, at);
        return false;
    }
    errorAt(t, codes.missingQuote, at);
    skipBogusDoctype(t);
    return false;
}

// Reads a doctype. The read point is past its "<!DOCTYPE", and its '<' is at `start`.
bool readDoctype(HtmlTokenizer* t, uint32_t start) {
    t->token.kind = HtmlTokenKind_Doctype;
    t->token.flags = HtmlTokenFlag_NoName | HtmlTokenFlag_NoPublicId | HtmlTokenFlag_NoSystemId;
    HtmlSpan name = begin(t->input.pos);
    HtmlSpan publicId = begin(t->input.pos);
    HtmlSpan systemId = begin(t->input.pos);
    auto emit = [&] { return emitDoctype(t, start, name, publicId, systemId); };
    auto eof = [&](uint32_t at) {
        errorAt(t, HtmlErrorCode_EofInDoctype, at);
        t->token.flags |= HtmlTokenFlag_ForceQuirks;
        return emit();
    };

    // html/#doctype-state: the whitespace that separates the keyword from the name.
    uint32_t byte = readByte(t, t->input.pos);
    if (byte == kEof)
        return eof(t->input.pos);
    if ((classOf(byte) & ByteClass_Space) == 0 && byte != '>')
        errorAt(t, HtmlErrorCode_MissingWhitespaceBeforeDoctypeName, t->input.pos);
    skipSpace(t);

    // html/#before-doctype-name-state: the name.
    uint32_t at = t->input.pos;
    byte = readByte(t, at);
    if (byte == '>') {
        errorAt(t, HtmlErrorCode_MissingDoctypeName, at);
        t->token.flags |= HtmlTokenFlag_ForceQuirks;
        t->input.pos = at + 1;
        return emit();
    }
    if (byte == kEof)
        return eof(at);
    t->token.flags &= ~HtmlTokenFlag_NoName;
    name = begin(at);
    if (readName(t, &name, ByteClass_NameEnd) == '>') {
        ++t->input.pos;
        return emit();
    }

    // html/#after-doctype-name-state: PUBLIC, SYSTEM, or content the doctype cannot carry.
    skipSpace(t);
    at = t->input.pos;
    byte = readByte(t, at);
    if (byte == '>') {
        t->input.pos = at + 1;
        return emit();
    }
    if (byte == kEof)
        return eof(at);

    PrStringView text = htmlInputText(t);
    PrStringView rest = stringSlice(text, at, text.size - at);
    bool isPublic = asciiStartsWithFold(rest, "public");
    bool isSystem = asciiStartsWithFold(rest, "system");
    if (!isPublic && !isSystem) {
        errorAt(t, HtmlErrorCode_InvalidCharacterSequenceAfterDoctypeName, at);
        t->token.flags |= HtmlTokenFlag_ForceQuirks;
        skipBogusDoctype(t);
        return emit();
    }
    t->input.pos = at + 6;

    if (isPublic) {
        if (!readIdentifier(t, kDoctypePublic, &publicId))
            return emit();

        // html/#after-doctype-public-identifier-state and html/#between-doctype-public-and-system-identifiers-state:
        // the system identifier may follow the public one.
        bool spaced = skipSpace(t);
        at = t->input.pos;
        byte = readByte(t, at);
        if (byte == '>') {
            t->input.pos = at + 1;
            return emit();
        }
        if (byte == kEof)
            return eof(at);
        if ((classOf(byte) & ByteClass_Quote) == 0) {
            errorAt(t, HtmlErrorCode_MissingQuoteBeforeDoctypeSystemIdentifier, at);
            t->token.flags |= HtmlTokenFlag_ForceQuirks;
            skipBogusDoctype(t);
            return emit();
        }
        if (!spaced)
            errorAt(t, HtmlErrorCode_MissingWhitespaceBetweenDoctypePublicAndSystemIdentifiers, at);
        t->input.pos = at + 1;
        t->token.flags &= ~HtmlTokenFlag_NoSystemId;
        systemId = begin(t->input.pos);
        if (!readIdentifierText(t, byte, &systemId, kDoctypeSystem.abrupt))
            return emit();
    } else if (!readIdentifier(t, kDoctypeSystem, &systemId)) {
        return emit();
    }

    // html/#after-doctype-system-identifier-state: what follows the system identifier. A doctype carries none of it.
    skipSpace(t);
    at = t->input.pos;
    byte = readByte(t, at);
    if (byte == '>') {
        t->input.pos = at + 1;
        return emit();
    }
    if (byte == kEof)
        return eof(at);
    errorAt(t, HtmlErrorCode_UnexpectedCharacterAfterDoctypeSystemIdentifier, at);
    skipBogusDoctype(t);
    return emit();
}

// ---------------------------------------------------------------------------------------------------------------------
// Processing instructions (html/#processing-instruction-open-state to html/#processing-instruction-questionable-state)
//
// An invalid target becomes a comment of that target with the question mark that started it. The question mark is in
// the input just before the target.

// Reads a processing instruction. The read point is past its "<?", and its '<' is at `start`.
bool readProcessingInstruction(HtmlTokenizer* t, uint32_t start) {
    PrStringView text = htmlInputText(t);
    uint32_t from = t->input.pos;
    uint32_t byte = readByte(t, from);
    if (byte == kEof) {
        errorAt(t, HtmlErrorCode_EofInProcessingInstruction, from);
        return emitEof(t);
    }
    if (!asciiIsLetter(asciiOf(byte)) && byte != '_') {
        errorAt(t, HtmlErrorCode_InvalidFirstCharacterOfProcessingInstructionTarget, from);
        return readBogusComment(t, start, HtmlSpan{from - 1, from});
    }

    uint32_t end = from;
    while (end < text.size) {
        uint32_t target = uint8_t(text.data[end]);
        if ((classOf(target) & ByteClass_Alnum) == 0 && target != '-' && target != '_')
            break;
        ++end;
    }
    t->input.pos = end;

    byte = readByte(t, end);
    if (byte == kEof) {
        errorAt(t, HtmlErrorCode_EofInProcessingInstruction, end);
        return emitEof(t);
    }
    if ((classOf(byte) & ByteClass_Space) == 0 && byte != '?' && byte != '>') {
        errorAt(t, HtmlErrorCode_InvalidProcessingInstructionTarget, end);
        return readBogusComment(t, start, HtmlSpan{from - 1, end});
    }

    PrStringView target = stringSlice(text, from, end - from);
    if (asciiEqualsFold(target, "xml") || asciiEqualsFold(target, "xml-stylesheet")) {
        errorAt(t, HtmlErrorCode_DisallowedProcessingInstructionTarget, end);
        return readBogusComment(t, start, HtmlSpan{from - 1, end});
    }
    HtmlSpan name = HtmlSpan{from, end};

    // html/#processing-instruction-data-state and html/#processing-instruction-questionable-state: the data, which ends
    // at a '>'. A '?' before that '>' is not part of it.
    skipSpace(t);
    HtmlSpan data = begin(t->input.pos);
    if (take(t, &data, ByteClass_Gt) == kEof) {
        errorAt(t, HtmlErrorCode_EofInProcessingInstruction, t->input.pos);
        return emitEof(t);
    }
    if (!htmlSpanEmpty(data) && text.data[t->input.pos - 1] == '?')
        --data.end;
    ++t->input.pos;
    return emitProcessingInstruction(t, start, name, data);
}

// ---------------------------------------------------------------------------------------------------------------------
// CDATA sections (html/#cdata-section-state to html/#cdata-section-end-state)

// Reads the characters of a CDATA section. A run of right square brackets closes the section with its last two
// brackets.
bool readCdataSection(HtmlTokenizer* t) {
    PrStringView text = htmlInputText(t);
    uint32_t start = t->input.pos;
    ByteClass seen = 0;
    for (;;) {
        seen |= scan(t, ByteClass_Bracket | ByteClass_Null);
        uint32_t at = t->input.pos;
        if (at >= text.size) {
            if (at != start)
                return emitRun(t, start, at, seen, HtmlState_CdataSection);
            errorAt(t, HtmlErrorCode_EofInCdata, at);
            return emitEof(t);
        }
        if (text.data[at] != ']') {
            if (at != start)
                return emitRun(t, start, at, seen, HtmlState_CdataSection);
            return emitNulls(t, false, HtmlState_CdataSection);
        }

        uint32_t end = at;
        while (end < text.size && text.data[end] == ']')
            ++end;
        if (end - at >= 2 && end < text.size && text.data[end] == '>') {
            uint32_t last = end - 2;
            if (last != at)
                seen |= ByteClass_Text;
            t->input.pos = end + 1;
            if (last == start) {
                t->state = HtmlState_Data;
                return false;
            }
            return emitCharacters(t, HtmlSpan{start, last}, HtmlSpan{start, end + 1}, textFlags(seen), HtmlState_Data);
        }
        seen |= ByteClass_Text;
        t->input.pos = end;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// What a '<' opens (html/#tag-open-state, html/#end-tag-open-state and html/#markup-declaration-open-state)

// Reads the markup declaration at the read point. Its "<!" is behind the read point, and its '<' is at `start`. Returns
// false if it started a CDATA section, which is characters rather than a token.
bool readMarkupDeclaration(HtmlTokenizer* t, uint32_t start) {
    PrStringView text = htmlInputText(t);
    uint32_t at = t->input.pos;
    PrStringView rest = stringSlice(text, at, text.size - at);
    if (stringStartsWith(rest, "--")) {
        t->input.pos = at + 2;
        return readComment(t, start);
    }
    if (asciiStartsWithFold(rest, "doctype")) {
        t->input.pos = at + 7;
        return readDoctype(t, start);
    }
    if (stringStartsWith(rest, "[CDATA[")) {
        t->input.pos = at + 7;
        if (t->foreign) {
            t->state = HtmlState_CdataSection;
            return false;
        }
        // The opening brackets are consumed, so the error is at the last of them.
        errorAt(t, HtmlErrorCode_CdataInHtmlContent, at + 6);
        return readBogusComment(t, start, HtmlSpan{at, at + 7});
    }

    // The state reads the byte before deciding what to do with it, so that the byte's code point errors come before
    // this error.
    if (at < text.size)
        check(t, at);
    errorAt(t, HtmlErrorCode_IncorrectlyOpenedComment, at);
    return readBogusComment(t, start, begin(at));
}

// Reads what follows the '<' at `start`, which is where the read point is. Returns false if it produced no token.
bool readTagOpen(HtmlTokenizer* t, uint32_t start) {
    uint32_t at = start + 1;
    t->input.pos = at;
    uint32_t byte = readByte(t, at);
    if (asciiIsLetter(asciiOf(byte)))
        return readTag(t, HtmlTokenKind_StartTag, start);
    if (byte == '!') {
        t->input.pos = at + 1;
        return readMarkupDeclaration(t, start);
    }
    if (byte == '?') {
        t->input.pos = at + 1;
        return readProcessingInstruction(t, start);
    }

    // html/#end-tag-open-state: the name of an end tag.
    if (byte == '/') {
        uint32_t after = readByte(t, at + 1);
        if (asciiIsLetter(asciiOf(after))) {
            t->input.pos = at + 1;
            return readTag(t, HtmlTokenKind_EndTag, start);
        }
        if (after == '>') {
            errorAt(t, HtmlErrorCode_MissingEndTagName, at + 1);
            t->input.pos = at + 2;
            return false;
        }
        if (after == kEof) {
            errorAt(t, HtmlErrorCode_EofBeforeTagName, at + 1);
            t->input.pos = at + 1;
            return emitRun(t, start, at + 1, ByteClass_Text, HtmlState_Data);
        }
        errorAt(t, HtmlErrorCode_InvalidFirstCharacterOfTagName, at + 1);
        t->input.pos = at + 1;
        return readBogusComment(t, start, begin(at + 1));
    }

    errorAt(t, byte == kEof ? HtmlErrorCode_EofBeforeTagName : HtmlErrorCode_InvalidFirstCharacterOfTagName, at);
    return emitRun(t, start, at, ByteClass_Text, HtmlState_Data);
}

// ---------------------------------------------------------------------------------------------------------------------
// Text (html/#data-state to html/#plaintext-state and html/#rcdata-less-than-sign-state to
// html/#script-data-double-escape-end-state)

// The data state. Its text ends at a '<', a '&' or a null.
bool readData(HtmlTokenizer* t) {
    uint32_t start = t->input.pos;
    ByteClass seen = scan(t, ByteClass_Lt | ByteClass_Amp | ByteClass_Null);
    uint32_t at = t->input.pos;
    if (at != start)
        return emitRun(t, start, at, seen, HtmlState_Data);

    uint32_t byte = readByte(t, at);
    if (byte == kEof)
        return emitEof(t);
    if (byte == 0)
        return emitNulls(t, true, HtmlState_Data);
    if (byte == '&') {
        readReference(t, nullptr, HtmlState_Data);
        return true;
    }
    return readTagOpen(t, at);
}

// The plaintext state. Its text ends only at a null.
bool readPlaintext(HtmlTokenizer* t) {
    uint32_t start = t->input.pos;
    ByteClass seen = scan(t, ByteClass_Null);
    uint32_t at = t->input.pos;
    if (at != start)
        return emitRun(t, start, at, seen, HtmlState_Plaintext);
    if (at >= t->input.text.count)
        return emitEof(t);
    return emitReplacement(t, HtmlState_Plaintext);
}

// The RCDATA and RAWTEXT states. Their text ends at the end tag of the element whose content the text is. Every other
// '<' is text.
bool readTextContent(HtmlTokenizer* t, HtmlState state) {
    ByteClass stop;
    if (state == HtmlState_Rcdata)
        stop = ByteClass(ByteClass_Lt | ByteClass_Amp | ByteClass_Null);
    else
        stop = ByteClass(ByteClass_Lt | ByteClass_Null);
    uint32_t start = t->input.pos;
    ByteClass seen = 0;
    for (;;) {
        seen |= scan(t, stop);
        uint32_t at = t->input.pos;
        uint32_t byte = readByte(t, at);
        if (byte == '<' && !isTextEndTag(t, at)) {
            seen |= ByteClass_Text;
            t->input.pos = at + 1;
            continue;
        }
        if (at != start)
            return emitRun(t, start, at, seen, state);
        if (byte == kEof)
            return emitEof(t);
        if (byte == 0)
            return emitReplacement(t, state);
        if (byte == '&') {
            readReference(t, nullptr, state);
            return true;
        }
        t->input.pos = at + 2;
        return readTag(t, HtmlTokenKind_EndTag, at);
    }
}

// The script data states. "<!--", "-->" and the nested <script> of a double escape move between them. The end tag of
// the script's element ends them. A double escape is the one state that the end tag does not close.
bool readScript(HtmlTokenizer* t) {
    PrStringView text = htmlInputText(t);
    HtmlState state = t->state;
    uint32_t start = t->input.pos;
    ByteClass seen = 0;
    uint32_t dashes = 0;
    for (;;) {
        ByteClass stop = ByteClass_Lt | ByteClass_Null;
        if (state != HtmlState_ScriptData)
            stop |= ByteClass_Dash | ByteClass_Gt;

        uint32_t from = t->input.pos;
        seen |= scan(t, stop);
        uint32_t at = t->input.pos;
        if (at != from)
            dashes = 0;

        if (at >= text.size) {
            if (at != start)
                return emitRun(t, start, at, seen, state);
            if (state != HtmlState_ScriptData)
                errorAt(t, HtmlErrorCode_EofInScriptHtmlCommentLikeText, at);
            return emitEof(t);
        }

        char byte = text.data[at];
        if (byte == '-') {
            ++dashes;
            seen |= ByteClass_Text;
            t->input.pos = at + 1;
            continue;
        }
        if (byte == '>') {
            // html/#script-data-escaped-dash-dash-state and html/#script-data-double-escaped-dash-dash-state: the "-->"
            // that ends the escape, however many dashes started it.
            if (dashes >= 2)
                state = HtmlState_ScriptData;
            dashes = 0;
            seen |= ByteClass_Text;
            t->input.pos = at + 1;
            continue;
        }
        dashes = 0;
        if (byte == '\0') {
            if (at != start)
                return emitRun(t, start, at, seen, state);
            t->state = state;
            return emitReplacement(t, state);
        }

        if (state != HtmlState_ScriptDataDoubleEscaped && isTextEndTag(t, at)) {
            if (at != start)
                return emitRun(t, start, at, seen, state);
            t->input.pos = at + 2;
            return readTag(t, HtmlTokenKind_EndTag, at);
        }
        seen |= ByteClass_Text;

        if (state == HtmlState_ScriptData) {
            // html/#script-data-escape-start-state to html/#script-data-escaped-state: the "<!--" that starts an
            // escape. The next '>' after its second dash closes the escape.
            if (at + 4 <= text.size && text.data[at + 1] == '!' && text.data[at + 2] == '-' && text.data[at + 3] == '-') {
                state = HtmlState_ScriptDataEscaped;
                dashes = 2;
                t->input.pos = at + 4;
                continue;
            }
            t->input.pos = at + 1;
            continue;
        }

        // html/#script-data-double-escape-start-state and html/#script-data-double-escape-end-state: a nested <script>
        // starts a double escape and its end tag ends one. Neither is a tag.
        uint32_t after = at + 1;
        bool closing = after < text.size && text.data[after] == '/';
        uint32_t name = closing ? after + 1 : after;
        uint32_t end = name;
        while (end < text.size && asciiIsLetter(text.data[end]))
            ++end;
        if (end != name && end < text.size && (classOf(text.data[end]) & ByteClass_TagEnd) != 0 &&
                closing == (state == HtmlState_ScriptDataDoubleEscaped) &&
                asciiEqualsFold(stringSlice(text, name, end - name), "script")) {
            state = closing ? HtmlState_ScriptDataEscaped : HtmlState_ScriptDataDoubleEscaped;
            t->input.pos = end;
            continue;
        }
        t->input.pos = end != name ? end : at + 1;
    }
}

// Inserts well-formed `text` into the input at the read point.
void insertText(HtmlTokenizer* tokenizer, PrStringView text) {
    Array<char>& input = tokenizer->input.text;
    uint32_t at = tokenizer->input.pos;

    // html/#preprocessing-the-input-stream normalizes the stream that the insertion joins, not the inserted text alone.
    // A carriage return at the end of the text pairs with a line feed at the read point. A line feed at the start of
    // the text pairs with a carriage return that ended the text inserted before it. Each pair is a single newline.
    uint32_t& carriageReturn = tokenizer->input.carriageReturn;
    if (text.size != 0 && text.data[0] == '\n' && carriageReturn != 0 && at == carriageReturn) {
        text.data += 1;
        text.size -= 1;
    }
    bool endsWithCarriageReturn = text.size != 0 && text.data[text.size - 1] == '\r';
    if (endsWithCarriageReturn && at < input.count && input.data[at] == '\n') {
        text.size -= 1;
        endsWithCarriageReturn = false;
    }
    if (text.size == 0)
        return;

    uint32_t room = uint32_t(text.size);

    input.reserve(input.count + room);
    memMove(input.data + at + room, input.data + at, input.count - at);

    uint32_t size = normalizeNewlines(input.data + at, text);
    if (size != room)
        memMove(input.data + at + size, input.data + at + room, input.count - at);
    input.count += size;
    if (endsWithCarriageReturn)
        carriageReturn = at + size;
    else if (carriageReturn != 0 && carriageReturn >= at)
        carriageReturn = carriageReturn == at ? 0 : carriageReturn + size;
}

// Runs one pass through the states, which ends when it has a token.
bool read(HtmlTokenizer* t) {
    switch (t->state) {
        case HtmlState_Rcdata:
        case HtmlState_Rawtext:                 return readTextContent(t, t->state);
        case HtmlState_ScriptData:
        case HtmlState_ScriptDataEscaped:
        case HtmlState_ScriptDataDoubleEscaped: return readScript(t);
        case HtmlState_Plaintext:               return readPlaintext(t);
        case HtmlState_CdataSection:            return readCdataSection(t);
        default:                                return readData(t);
    }
}

// Returns true if the token just read, which the tokenizer started in `state` at `start`, may have been cut short by
// the end of an input that more input may follow. This is true for a token that the end of the input decided
// (HtmlTokenizer::cut). It is also true for a token that reached the end of the input, except a markup token closed by
// its '>', and the text of the data state, whose characters are final. A run of that text ends before a '<' or a '&',
// so a character token of the data state that starts at one of them is the tag or the character reference that the end
// of the input cut short.
bool reachedEnd(const HtmlTokenizer* t, HtmlState state, uint32_t start) {
    const HtmlInput& input = t->input;
    if (t->cut)
        return true;
    if (input.pos < input.text.count)
        return false;
    switch (t->token.kind) {
        case HtmlTokenKind_Character:
            return state != HtmlState_Data || input.text.data[start] == '<' || input.text.data[start] == '&';
        case HtmlTokenKind_Eof:
            return true;
        default:
            return input.pos == 0 || input.text.data[input.pos - 1] != '>';
    }
}

} // namespace

void htmlTokenizerStart(HtmlTokenizer* tokenizer, PrStringView source, Array<HtmlError>* errors) {
    TextBuffer repaired;
    PrStringView text = wellFormed(source, &repaired);
    tokenizer->input.text.count = 0;
    if (text.size != 0) {
        tokenizer->input.text.reserve(uint32_t(text.size));
        tokenizer->input.text.count = normalizeNewlines(tokenizer->input.text.data, text);
    }
    textReset(&repaired);

    tokenizer->input.pos = 0;
    tokenizer->input.carriageReturn = 0;
    tokenizer->values.count = 0;
    tokenizer->attributes.count = 0;
    tokenizer->errors = errors;
    tokenizer->token = HtmlToken{};
    tokenizer->checked = 0;
    tokenizer->state = HtmlState_Data;
    tokenizer->foreign = false;
    prStringReset(&tokenizer->lastStartTag);
}

bool htmlTokenizerNext(HtmlTokenizer* tokenizer) {
    uint32_t pos = tokenizer->input.pos;
    uint32_t checked = tokenizer->checked;
    uint32_t errors = tokenizer->errors->count;
    HtmlState state = tokenizer->state;
    tokenizer->values.count = 0;
    tokenizer->token = HtmlToken{};
    tokenizer->cut = false;
    while (!read(tokenizer)) {
    }
    if (!tokenizer->open || !reachedEnd(tokenizer, state, pos))
        return true;
    tokenizer->input.pos = pos;
    tokenizer->checked = checked;
    tokenizer->errors->count = errors;
    tokenizer->state = state;
    tokenizer->values.count = 0;
    tokenizer->attributes.count = 0;
    tokenizer->token = HtmlToken{};
    return false;
}

void htmlTokenizerSetState(HtmlTokenizer* tokenizer, HtmlState state, PrStringView tag) {
    tokenizer->state = state;
    prStringReplace(&tokenizer->lastStartTag, tag);
}

void htmlTokenizerInsert(HtmlTokenizer* tokenizer, PrStringView text) {
    TextBuffer repaired;
    insertText(tokenizer, wellFormed(text, &repaired));
    textReset(&repaired);
}

void htmlTokenizerAppend(HtmlTokenizer* tokenizer, PrStringView text) {
    PR_ASSERT(tokenizer->open);
    uint32_t pos = tokenizer->input.pos;
    tokenizer->input.pos = tokenizer->input.text.count;
    htmlTokenizerInsert(tokenizer, text);
    tokenizer->input.pos = pos;
}

bool htmlTokenizerIsIdle(const HtmlTokenizer* tokenizer) {
    return tokenizer->state == HtmlState_Data && tokenizer->input.pos == tokenizer->input.text.count;
}

void htmlTokenizerMakeTag(HtmlTokenizer* tokenizer, HtmlTokenKind kind, PrStringView name) {
    PR_ASSERT(kind == HtmlTokenKind_StartTag || kind == HtmlTokenKind_EndTag);
    tokenizer->values.count = 0;
    tokenizer->attributes.count = 0;
    tokenizer->token = HtmlToken{};
    tokenizer->token.kind = kind;
    tokenizer->token.span = HtmlSpan{tokenizer->input.pos, tokenizer->input.pos};
    tokenizer->token.name = HtmlSpan{kHtmlSpanValues, 0};
    appendText(tokenizer, &tokenizer->token.name, name);
}

void htmlTokenizerMakeCharacters(HtmlTokenizer* tokenizer, PrStringView text) {
    tokenizer->values.count = 0;
    tokenizer->attributes.count = 0;
    tokenizer->token = HtmlToken{};
    TextBuffer repaired;
    PrStringView formed = wellFormed(text, &repaired);
    tokenizer->values.reserve(uint32_t(formed.size));
    tokenizer->values.count = normalizeNewlines(tokenizer->values.data, formed);
    textReset(&repaired);
    PrStringView written{tokenizer->values.data, tokenizer->values.count};
    PR_ASSERT(stringFind(written, '\0') == written.size);
    tokenizer->token.kind = HtmlTokenKind_Character;
    tokenizer->token.flags = textFlags(classesOf(written));
    tokenizer->token.span = HtmlSpan{tokenizer->input.pos, tokenizer->input.pos};
    tokenizer->token.data = HtmlSpan{kHtmlSpanValues, tokenizer->values.count};
}

void htmlTokenizerAddAttribute(HtmlTokenizer* tokenizer, PrStringView name, PrStringView value) {
    PR_ASSERT(tokenizer->token.kind == HtmlTokenKind_StartTag || tokenizer->token.kind == HtmlTokenKind_EndTag);
    HtmlAttribute attribute{};
    attribute.name = HtmlSpan{tokenizer->values.count | kHtmlSpanValues, tokenizer->values.count};
    appendText(tokenizer, &attribute.name, name);
    TextBuffer repaired;
    PrStringView formed = wellFormed(value, &repaired);
    uint32_t start = tokenizer->values.count;
    tokenizer->values.reserve(start + uint32_t(formed.size));
    tokenizer->values.count = start + normalizeNewlines(tokenizer->values.data + start, formed);
    textReset(&repaired);
    attribute.value = HtmlSpan{start | kHtmlSpanValues, tokenizer->values.count};
    tokenizer->attributes.push(attribute);
}

void htmlTokenizerReset(HtmlTokenizer* tokenizer) {
    tokenizer->input.text.reset();
    tokenizer->values.reset();
    tokenizer->attributes.reset();
    prStringReset(&tokenizer->lastStartTag);
    *tokenizer = HtmlTokenizer{};
}

PrStringView htmlSpanText(const HtmlTokenizer* tokenizer, HtmlSpan span) {
    const Array<char>& from = (span.start & kHtmlSpanValues) != 0 ? tokenizer->values : tokenizer->input.text;
    uint32_t start = span.start & ~kHtmlSpanValues;
    return PrStringView{from.data + start, span.end - start};
}

uint32_t htmlTokenRun(const HtmlTokenizer* tokenizer, const HtmlToken* token, HtmlTokenFlags flags) {
    PrStringView text = htmlSpanText(tokenizer, token->data);
    uint32_t count = 0;
    while (count < text.size && (textFlags(classOf(text.data[count])) & flags) != 0)
        ++count;
    return count;
}

void htmlTokenAdvance(const HtmlTokenizer* tokenizer, HtmlToken* token, uint32_t count) {
    if (count == 0)
        return;

    bool values = (token->data.start & kHtmlSpanValues) != 0;
    token->data.start += count;
    if (!values)
        token->span.start = token->data.start;
    token->flags = textFlags(classesOf(htmlSpanText(tokenizer, token->data)));
}

void htmlInputPosition(PrStringView text, uint32_t* at, uint32_t to, uint32_t* line, uint32_t* column) {
    uint32_t i = *at;
    while (i < to && i < text.size) {
        if (text.data[i] == '\n') {
            ++*line;
            *column = 1;
            ++i;
            continue;
        }

        uint32_t codePoint = 0;
        i += prUtf8Decode(text, i, &codePoint);
        *column += codePoint >= 0x10000 ? 2 : 1;
    }
    *at = i;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
