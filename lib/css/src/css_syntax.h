// Tokens, and reading them
//
// css-syntax-3 - https://drafts.csswg.org/css-syntax/

#pragma once

#include "pr/array.h"
#include "pr/charset.h"
#include "pr/reserved_array.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"
#include "pr/utf8.h"

#include "css/src/css_keyword.h"
#include "css/src/css_unit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Decoding (css-syntax-3/#input-byte-stream)

// Returns the text of a style sheet's bytes as UTF-8 (css-syntax-3/#css-decode-bytes). The charset that decodes them is
// that of the byte order mark, or else `protocol`, the charset of the sheet's Content-Type, or else the one that an
// @charset rule opening the sheet names, or else `environment`, the charset of the document that loads the sheet, or
// else UTF-8. The text is a view of `bytes` if they decode to themselves, and is otherwise appended to `out`.
PrStringView cssDecodeBytes(PrStringView bytes, PrCharset protocol, PrCharset environment, PrString* out);

// ---------------------------------------------------------------------------------------------------------------------
// Byte classes (css-syntax-3/#tokenizer-definitions)

// The classes of a byte, for the per-byte tests of the tokenizer and the serializer.
enum {
    CssByteFlag_Ident = 1 << 0,       // an ASCII byte that continues an ident sequence
    CssByteFlag_Start = 1 << 1,       // an ASCII byte that may start an ident sequence

    // Whitespace, including the carriage return and form feed that css-syntax-3/#input-preprocessing rewrites.
    CssByteFlag_Space = 1 << 2,
    CssByteFlag_Newline = 1 << 3,     // a line feed, carriage return or form feed
    CssByteFlag_Decode = 1 << 4,      // a null or a byte above ASCII, which the decoder resolves

    // A null, a carriage return or a form feed (css-syntax-3/#input-preprocessing).
    CssByteFlag_Rewritten = 1 << 5,
    CssByteFlag_Control = 1 << 6,     // a C0 control or a delete, which serializing escapes

    // A byte that an unquoted url may not hold (css-syntax-3/#consume-url-token).
    CssByteFlag_UrlEnd = 1 << 7,
};
typedef uint8_t CssByteFlags;

// The classes of every byte, indexed by the byte.
struct CssByteTable {
    CssByteFlags of[256];
};
extern const CssByteTable kCssByte;

// Returns true if a code point is a C0 control or a delete, which serializing escapes.
inline bool cssIsControl(uint32_t codePoint) {
    return codePoint < 0x80 && (kCssByte.of[codePoint] & CssByteFlag_Control) != 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Tokens (css-syntax-3/#tokenization)

// A token's lexical form. The entry past the last token holds CssTokenKind_Eof.
enum {
    CssTokenKind_Eof,

    CssTokenKind_Ident,          // <ident-token>
    CssTokenKind_Function,       // <function-token>, an ident immediately followed by '('
    CssTokenKind_AtKeyword,      // <at-keyword-token>
    CssTokenKind_Hash,           // <hash-token>
    CssTokenKind_String,         // <string-token>
    CssTokenKind_BadString,      // <bad-string-token>
    CssTokenKind_Url,            // <url-token>, the unquoted form only
    CssTokenKind_BadUrl,         // <bad-url-token>
    CssTokenKind_Delim,          // <delim-token>

    CssTokenKind_Number,         // <number-token>
    CssTokenKind_Percentage,     // <percentage-token>
    CssTokenKind_Dimension,      // <dimension-token>
    CssTokenKind_UnicodeRange,   // <unicode-range-token>

    CssTokenKind_Whitespace,     // <whitespace-token>, one per run
    CssTokenKind_Cdo,            // <CDO-token>
    CssTokenKind_Cdc,            // <CDC-token>

    CssTokenKind_Colon,
    CssTokenKind_Semicolon,
    CssTokenKind_Comma,
    CssTokenKind_OpenBracket,
    CssTokenKind_CloseBracket,
    CssTokenKind_OpenParen,
    CssTokenKind_CloseParen,
    CssTokenKind_OpenBrace,
    CssTokenKind_CloseBrace,

    // A value already parsed, in place of the tokens that would write it. The token's closer is the index of the value
    // in the array's `values`.
    CssTokenKind_Value,

    CssTokenKind_Count,
};
typedef uint8_t CssTokenKind;

// The tokens that one byte forms on its own (css-syntax-3/#consume-token), and the bytes that form them.
struct CssPunctuationTable {
    CssTokenKind kindOf[256];           // the punctuation token of a byte, or CssTokenKind_Delim
    char byteOf[CssTokenKind_Count];    // the byte of a punctuation token, or 0 for another kind
};
extern const CssPunctuationTable kCssPunctuation;

// The properties of one token.
enum {
    CssTokenFlag_Id = 1 << 0,            // a <hash-token> whose value is a valid identifier
    CssTokenFlag_Integer = 1 << 1,       // a numeric token written without '.' or an exponent
    CssTokenFlag_SignPresent = 1 << 2,   // a numeric token that starts with '+' or '-'
    CssTokenFlag_EofTerminated = 1 << 3, // ended at the end of input rather than at its own terminator
    CssTokenFlag_Verbatim = 1 << 4,      // the bytes that the token covers are its serialization
    CssTokenFlag_Contiguous = 1 << 5,    // starts where the token before it ended, with no comment
    CssTokenFlag_PlainText = 1 << 6,     // the bytes are the token's value, with no escape to resolve
    CssTokenFlag_Unmatched = 1 << 7,     // a close token that no opener before it matches

    CssTokenFlag_PlainVerbatim = CssTokenFlag_PlainText | CssTokenFlag_Verbatim,
};
typedef uint8_t CssTokenFlags;

// Options for how cssTokenize reads its source. `UnicodeRanges` is the "unicode ranges allowed" flag of
// css-syntax-3/#consume-token, which only the value of the unicode-range descriptor sets.
enum {
    CssTokenizeFlag_UnicodeRanges = 1 << 0,
};
typedef uint8_t CssTokenizeFlags;

// The scratch state that tokenizing keeps besides its result, reused from one input to the next.
struct CssTokenScratch {
    Array<uint32_t> open;   // the openers waiting for a closer
};

// A node of a parsed value (css_value.h).
struct CssValueNode;

// The tokens of a run of source, read through the functions below. Each token is a span of `source`, the bytes given to
// cssTokenize. Comments are not tokens. Each array holds one property of every token. All arrays have one entry past
// the count. In that entry, the kind is CssTokenKind_Eof, the start and end are the size of the source, the unit is
// PrCssUnit_Number, the flags are zero and the closer is the count.
struct CssTokenArray {
    PrStringView source;
    const CssValueNode* PR_NULLABLE values;   // the values that CssTokenKind_Value tokens stand for (cssValueTokens)
    const char* PR_NULLABLE valueText;        // the text that `values` references
    ReservedArray<uint32_t> starts;           // the first byte of the token
    ReservedArray<uint32_t> ends;             // one past the last byte of the token
    ReservedArray<CssTokenKind> kinds;
    ReservedArray<CssTokenFlags> flags;
    ReservedArray<PrCssUnit> units;             // a dimension's PrCssUnit, unread for other kinds

    // For an opener, the matching close token, or the count if the input never closed it. For a CssTokenKind_Value
    // token, the index in `values`. Unread for other kinds.
    ReservedArray<uint32_t> closers;
};

// Tokenizes `source`, replacing whatever `tokens` held. `source` may be any bytes. An ill-formed UTF-8 sequence in
// `source` reads as U+FFFD.
void cssTokenize(CssTokenArray* tokens, CssTokenScratch* scratch, PrStringView source, CssTokenizeFlags flags = 0);

// Frees the scratch's memory.
void cssTokenScratchReset(CssTokenScratch* scratch);

// Frees the tokens' memory and empties `tokens`.
void cssTokenArrayReset(CssTokenArray* tokens);

// ---------------------------------------------------------------------------------------------------------------------
// Reading a token
//
// Every index up to and including the count reads an entry. The entry at the count is the Eof.

// Returns the number of tokens, not counting the Eof entry.
inline uint32_t cssTokenCount(const CssTokenArray* tokens) {
    return tokens->kinds.count;
}

// Returns the kind and the flags of the token at `index`.
inline CssTokenKind cssTokenKind(const CssTokenArray* tokens, uint32_t index) {
    return tokens->kinds.data[index];
}
inline CssTokenFlags cssTokenFlags(const CssTokenArray* tokens, uint32_t index) {
    return tokens->flags.data[index];
}

// Returns the bytes that the token at `index` covers.
inline PrStringView cssTokenText(const CssTokenArray* tokens, uint32_t index) {
    uint32_t start = tokens->starts.data[index];
    return PrStringView{tokens->source.data + start, tokens->ends.data[index] - start};
}

// Returns the unit of the dimension token at `index`, or PrCssUnit_Unknown for a unit that no table names. The token at
// `index` must be a dimension.
inline PrCssUnit cssTokenUnit(const CssTokenArray* tokens, uint32_t index) {
    return tokens->units.data[index];
}

// Returns the close token of the block that the opener at `index` starts, or the count if the input never closed it.
// The result is meaningless for anything but an opener.
inline uint32_t cssTokenCloser(const CssTokenArray* tokens, uint32_t index) {
    return tokens->closers.data[index];
}

// Returns the index one past the close token of the block that the opener at `index` starts, or the count if the input
// never closed it. The token at `index` must be an opener.
inline uint32_t cssTokenBlockEnd(const CssTokenArray* tokens, uint32_t index) {
    uint32_t closer = tokens->closers.data[index];
    return closer < tokens->kinds.count ? closer + 1 : tokens->kinds.count;
}

// Returns the bytes of the token at `index`, without the syntax that its kind writes around its value: a string's
// quotes, the first byte of a hash or an at-keyword, a function's '(', and a url's url( ) and the whitespace inside. If
// the token is CssTokenFlag_PlainText, the bytes are the value. Otherwise cssValue resolves them to the value.
PrStringView cssTokenValueText(const CssTokenArray* tokens, uint32_t index);

// Steps `pos` over one code point of the value of `text`, which is its bytes with escapes resolved
// (css-syntax-3/#consume-escaped-code-point) and preprocessing applied (css-syntax-3/#input-preprocessing). A line
// continuation is part of no code point, so a step may consume bytes and produce none. Returns false at the end.
bool cssValueNext(PrStringView text, uint32_t* pos, uint32_t* codePoint);

// Writes the value of `text` into `buffer`, as cssValueNext reads it. Returns the size written, or, if `buffer` is too
// small, the size that it would need.
uint32_t cssValue(PrStringView text, char* PR_NULLABLE buffer, uint32_t capacity);

// Appends the value of `text` to `out`, as cssValueNext reads it.
void cssValueAppend(PrTextBuffer* out, PrStringView text);

// Returns the keyword that the token at `index` names, or PrCssKeyword_Unknown. The comparison uses the token's value,
// so an escape matches the code point that it spells.
PrCssKeyword cssTokenKeyword(const CssTokenArray* tokens, uint32_t index);

// Returns true if the token at `index` is an ident whose value starts with two hyphens. Such a token is a
// <dashed-ident>, which names a custom property (css-values-4/#dashed-idents).
bool cssTokenIsDashedIdent(const CssTokenArray* tokens, uint32_t index);

// Returns the value of the numeric token at `index`, parsed from the bytes that it covers
// (css-syntax-3/#consume-number). A token keeps the text that it was written as, not the number that it parsed to.
double cssTokenNumber(const CssTokenArray* tokens, uint32_t index);

// Returns the bytes of the unit of the dimension token at `index`: its text without the number.
PrStringView cssTokenUnitText(const CssTokenArray* tokens, uint32_t index);

// Returns the code point of the delim token at `index`.
inline uint32_t cssTokenDelim(const CssTokenArray* tokens, uint32_t index) {
    uint32_t start = tokens->starts.data[index];
    uint8_t lead = uint8_t(tokens->source.data[start]);
    if (lead < 0x80)
        return lead;
    uint32_t codePoint = 0;
    prUtf8DecodeScalar(tokens->source, start, &codePoint);
    return codePoint;
}

// Writes the code points that the unicode-range token at `index` covers, parsed from its bytes
// (css-syntax-3/#consume-unicode-range-token). `last` may be before `first`.
void cssTokenRange(const CssTokenArray* tokens, uint32_t index, uint32_t* first, uint32_t* last);

// ---------------------------------------------------------------------------------------------------------------------
// Streams
//
// A parser marks a stream's position, reads, and resets to the mark. It reads a span that it kept again by opening a
// stream over the span.

// A run of tokens that ends at `end`, and a position `pos` in it.
struct CssTokenStream {
    const CssTokenArray* tokens;
    uint32_t pos;
    uint32_t end;
};

// A run of tokens held without interpretation.
struct CssTokenSpan {
    uint32_t start;
    uint32_t count;
};

// Returns a stream over every token of `tokens`, positioned at the first.
inline CssTokenStream cssStream(const CssTokenArray* tokens) {
    return CssTokenStream{tokens, 0, cssTokenCount(tokens)};
}

// Returns a stream over `span` alone, positioned at its first token.
inline CssTokenStream cssSubStream(const CssTokenStream* ts, CssTokenSpan span) {
    return CssTokenStream{ts->tokens, span.start, span.start + span.count};
}

// Returns the tokens from the stream's position to its end.
inline CssTokenSpan cssStreamAll(const CssTokenStream* ts) {
    return CssTokenSpan{ts->pos, ts->end - ts->pos};
}

// Returns the kind at the stream's position, or CssTokenKind_Eof at its end.
inline CssTokenKind cssPeek(const CssTokenStream* ts) {
    if (ts->pos == ts->end)
        return CssTokenKind_Eof;
    return cssTokenKind(ts->tokens, ts->pos);
}

// Returns true if the token at the stream's position is a delim of `codePoint`.
bool cssPeekDelim(const CssTokenStream* ts, uint32_t codePoint);

// Steps past the token at the stream's position and returns its index. At the end, returns the end and does not move.
inline uint32_t cssNext(CssTokenStream* ts) {
    uint32_t index = ts->pos;
    if (ts->pos < ts->end)
        ++ts->pos;
    return index;
}

// Returns the stream's position, for cssReset.
inline uint32_t cssMark(const CssTokenStream* ts) { return ts->pos; }

// Moves the stream back to `mark`, a position that cssMark returned.
inline void cssReset(CssTokenStream* ts, uint32_t mark) { ts->pos = mark; }

// Steps past whitespace tokens.
inline void cssSkipWhitespace(CssTokenStream* ts) {
    while (cssPeek(ts) == CssTokenKind_Whitespace)
        ++ts->pos;
}

// Returns true if only whitespace is left. Steps past that whitespace.
bool cssAtEnd(CssTokenStream* ts);

// Returns true if a comma follows at the stream's level, before a '{' or a closer that the stream did not open. Writes
// the comma's index to `out`. The stream does not move.
bool cssNextComma(const CssTokenStream* ts, uint32_t* out);

// Returns the end of the block that the stream is in: the first closer that the stream did not open, or the stream's
// end.
uint32_t cssBlockEnd(const CssTokenStream* ts);

// Returns `span` with its leading and trailing whitespace tokens dropped.
CssTokenSpan cssTrimWhitespace(const CssTokenArray* tokens, CssTokenSpan span);

// Returns the stream's remaining tokens, trimmed in the same way as cssTrimWhitespace.
inline CssTokenSpan cssStreamTrimmed(const CssTokenStream* ts) {
    return cssTrimWhitespace(ts->tokens, cssStreamAll(ts));
}

// Returns the bytes that `span` covers, from its first token to the end of its last, including comments.
PrStringView cssSpanText(const CssTokenArray* tokens, CssTokenSpan span);

// Returns true if the source that `span` covers is its own serialization. That holds if every token is verbatim and no
// comment was skipped inside the run. Writes those bytes to `out`. An empty span leaves `out` empty.
bool cssSpanIsVerbatim(const CssTokenArray* tokens, CssTokenSpan span, PrStringView* out);

// ---------------------------------------------------------------------------------------------------------------------
// Component values (css-syntax-3/#parsing)

// Consumes a component value: one token, or a block and everything it balances (css-syntax-3/#consume-component-value).
// Returns false at the end of the stream.
bool cssConsumeComponentValue(CssTokenStream* ts, CssTokenSpan* out);

// Consumes a <declaration-value>: component values up to a top-level ';' or '!', or a closing bracket this run did not
// open. Returns false if the run would be empty.
bool cssConsumeDeclarationValue(CssTokenStream* ts, CssTokenSpan* out);

// Consumes an <any-value>. It is consumed in the same way as a <declaration-value>, except that ';' and '!' are part of
// it.
bool cssConsumeAnyValue(CssTokenStream* ts, CssTokenSpan* out);

// ---------------------------------------------------------------------------------------------------------------------
// An+B (css-syntax-3/#anb-microsyntax)

// Consumes an <an+b>, writing the two coefficients. On failure, leaves the stream unmodified.
bool cssConsumeAnPlusB(CssTokenStream* ts, int32_t* a, int32_t* b);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
