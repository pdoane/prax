// Tokenization
//
// html/#tokenization - https://html.spec.whatwg.org/multipage/parsing.html#tokenization

#pragma once

#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"

#include "html/src/html_error.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// The input stream (html/#preprocessing-the-input-stream)

// The tokenizer's copy of the stream that html/#tokenization tokenizes, so that a position in the copy is a position in
// the stream. The copy is UTF-8, with U+FFFD for each ill-formed sequence (encoding/#utf-8-decoder) and with carriage
// returns rewritten. A surrogate code point is stored as the three bytes that UTF-8 would encode it with, and
// html/#parse-errors reports it as surrogate-in-input-stream.
//
// A token refers to bytes of the copy by offset. An insertion at the read point moves nothing before that point, so an
// offset stays valid across an insertion.
struct HtmlInput {
    RawArray<char> text;
    uint32_t pos;            // the next byte to read

    // If the last inserted text ended with a carriage return, the position just past the newline that it became. A line
    // feed inserted at that position pairs with it. Zero if there is none.
    uint32_t carriageReturn;
};

// A run of bytes of the input, or of the tokenizer's values if `start` has kHtmlSpanValues set. An array's capacity
// stays below kArrayMaxGrowth, so an offset into the input never has that bit set.
struct HtmlSpan {
    uint32_t start;
    uint32_t end;            // one past the last byte
};

// The bit in a span's start that marks the span as a run of the values rather than of the input.
constexpr uint32_t kHtmlSpanValues = 0x80000000u;

// ---------------------------------------------------------------------------------------------------------------------
// Tokens

// The kinds of token that the tokenizer emits (html/#tokenization).
enum {
    HtmlTokenKind_Eof,
    HtmlTokenKind_Doctype,
    HtmlTokenKind_StartTag,
    HtmlTokenKind_EndTag,
    HtmlTokenKind_Comment,
    HtmlTokenKind_ProcessingInstruction,
    HtmlTokenKind_Character,
    HtmlTokenKind_Count,
};
typedef uint8_t HtmlTokenKind;

// How a token was written. For a character token, the flags say what kinds of character it holds.
enum {
    HtmlTokenFlag_SelfClosing = 1 << 0,   // a tag written with a trailing '/'
    HtmlTokenFlag_ForceQuirks = 1 << 1,   // a doctype's force-quirks flag
    HtmlTokenFlag_NoName = 1 << 2,        // a doctype written without a name
    HtmlTokenFlag_NoPublicId = 1 << 3,    // a doctype written without a public identifier
    HtmlTokenFlag_NoSystemId = 1 << 4,    // a doctype written without a system identifier
    HtmlTokenFlag_Whitespace = 1 << 5,    // a character token holding ASCII whitespace
    HtmlTokenFlag_Text = 1 << 6,          // a character token holding text other than ASCII whitespace and nulls
    HtmlTokenFlag_Null = 1 << 7,          // a character token holding a null
};
typedef uint8_t HtmlTokenFlags;

// An attribute of a start tag, as written.
struct HtmlAttribute {
    HtmlSpan name;
    HtmlSpan value;
};

// A token. The attributes of a tag are in the tokenizer's `attributes`.
struct HtmlToken {
    HtmlTokenKind kind;
    HtmlTokenFlags flags;
    HtmlSpan span;           // the bytes of the input that the token was written with
    HtmlSpan name;           // the lowercased name of a tag or a doctype, or the target of a processing instruction
    HtmlSpan data;           // the data of a comment or a processing instruction, or a character token's characters
    HtmlSpan publicId;       // the public identifier of a doctype
    HtmlSpan systemId;       // the system identifier of a doctype
};

// ---------------------------------------------------------------------------------------------------------------------
// The tokenizer

// The states that the tokenizer can be in between tokens. htmlTokenizerSetState accepts any state before
// ScriptDataEscaped. Apart from Data, those are the states that html/#parsing-elements-that-contain-only-text switches
// to for an element whose content is text, and the CDATA section that a foreign element allows. The states from
// ScriptDataEscaped onward are reached from inside script data, and differ from it only in what a '<' starts.
enum {
    HtmlState_Data,
    HtmlState_Rcdata,
    HtmlState_Rawtext,
    HtmlState_ScriptData,
    HtmlState_Plaintext,
    HtmlState_CdataSection,
    HtmlState_ScriptDataEscaped,
    HtmlState_ScriptDataDoubleEscaped,
    HtmlState_Count,
};
typedef uint8_t HtmlState;

// The name of each state, spelled as in the spec, in lowercase with hyphens.
extern const PrStringView kHtmlStateName[HtmlState_Count];

// The tokenizer's input, its current token, and the state in which it reads the next token.
struct HtmlTokenizer {
    HtmlInput input;

    // The text of a token if it is not the input's own bytes. Spans with kHtmlSpanValues set point into it.
    RawArray<char> values;

    HtmlToken token;                       // written by htmlTokenizerNext
    RawArray<HtmlAttribute> attributes;    // the attributes of the start tag being read, in written order
    RawArray<HtmlError>* errors;           // the caller's list of errors, set by htmlTokenizerStart

    // The tag that an end tag must match to be appropriate (html/#tokenization). This is the element whose content the
    // state was entered for.
    PrRawString lastStartTag;

    // The number of input bytes whose code point errors (html/#preprocessing-the-input-stream) have been reported.
    uint32_t checked;

    HtmlState state;

    // True if the adjusted current node is a foreign element, which lets `<![CDATA[` start a section
    // (html/#markup-declaration-open-state).
    bool foreign;

    // True if more input may follow the current input, as for a script-created parser until it is closed
    // (html/#script-created-parser).
    bool open;

    // True if the end of the input decided the current token. Either an eof-in-* error ended the token, or the name of
    // a named reference ran to the end of the input, and more input could have continued the name.
    bool cut;
};

// Tokenizes `source`, replacing the tokenizer's previous input. `source` may be any bytes, and an ill-formed UTF-8
// sequence in it is read as U+FFFD. Errors are appended to `errors`, which the caller owns and which must outlive the
// tokenizing.
void htmlTokenizerStart(HtmlTokenizer* tokenizer, PrStringView source, RawArray<HtmlError>* errors);

// Reads the next token. At the end of the input, and at every call after it, the token is HtmlTokenKind_Eof. A
// character token holds every character that the state read in one run. Returns false, with nothing read, if the input
// is open and ends before the token does. The tokenizer reads the token again once more input follows or the input
// closes.
bool htmlTokenizerNext(HtmlTokenizer* tokenizer);

// Sets the state in which the next token is read, and the element whose content that state holds
// (html/#parsing-elements-that-contain-only-text). `tag` is the name that an end tag must have to end the state, or the
// null view for a state that no end tag ends.
void htmlTokenizerSetState(HtmlTokenizer* tokenizer, HtmlState state, PrStringView tag);

// Inserts `text` at the tokenizer's read position, for document.write() (html/#document.write()). Like the source,
// `text` may be any bytes.
void htmlTokenizerInsert(HtmlTokenizer* tokenizer, PrStringView text);

// Appends `text` to the end of the input, which must be open.
void htmlTokenizerAppend(HtmlTokenizer* tokenizer, PrStringView text);

// Returns true if the tokenizer is in the data state and has read all of its input. A token that a caller makes is then
// the token that the tokenizer would read next from the input that the token is written with.
bool htmlTokenizerIsIdle(const HtmlTokenizer* tokenizer);

// Make the tokenizer's token one that a caller writes rather than one that the tokenizer reads, as at the read point: a
// start or an end tag of `kind` named `name`, which is lowercase, with no attributes, or a character token of `text`.
// The token's text is in the values, and replaces what they held. Like the input, the text may be any bytes, and its
// carriage returns are rewritten. It holds no U+0000.
void htmlTokenizerMakeTag(HtmlTokenizer* tokenizer, HtmlTokenKind kind, PrStringView name);
void htmlTokenizerMakeCharacters(HtmlTokenizer* tokenizer, PrStringView text);

// Adds the attribute `name`, which is lowercase, with `value` to the tag that htmlTokenizerMakeTag made. The tag must
// not have an attribute of that name already.
void htmlTokenizerAddAttribute(HtmlTokenizer* tokenizer, PrStringView name, PrStringView value);

// Frees everything that the tokenizer owns, and leaves it empty.
void htmlTokenizerReset(HtmlTokenizer* tokenizer);

// ---------------------------------------------------------------------------------------------------------------------
// Reading a token

// Returns the text that `span` covers, from the input or from the values according to the span's bit. A span into the
// input is valid as long as the tokenizer is. A span into the values is valid until the next htmlTokenizerNext, which
// clears the values.
PrStringView htmlSpanText(const HtmlTokenizer* tokenizer, HtmlSpan span);

// Returns true if `span` covers no text.
inline bool htmlSpanEmpty(HtmlSpan span) {
    return (span.start & ~kHtmlSpanValues) == span.end;
}

// Returns the number of bytes at the start of a character token's characters that have one of `flags`. Whitespace is
// tab, line feed, form feed, carriage return and space. A carriage return can reach a token only through `&#13;`,
// because preprocessing removed carriage returns from the input.
uint32_t htmlTokenRun(const HtmlTokenizer* tokenizer, const HtmlToken* token, HtmlTokenFlags flags);

// Drops the first `count` bytes of a character token's characters, along with their flags and the input they were
// written with.
void htmlTokenAdvance(const HtmlTokenizer* tokenizer, HtmlToken* token, uint32_t count);

// Returns the current input, which an error's offset and a token's span point into.
inline PrStringView htmlInputText(const HtmlTokenizer* tokenizer) {
    return PrStringView{tokenizer->input.text.data, tokenizer->input.text.count};
}

// Advances `line` and `column` from byte `at` to byte `to` of an input stream. A code point outside the Basic
// Multilingual Plane counts two columns, and every other code point one. `line` and `column` start at one and `at` at
// zero. Successive calls advance in input order.
void htmlInputPosition(PrStringView text, uint32_t* at, uint32_t to, uint32_t* line, uint32_t* column);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
