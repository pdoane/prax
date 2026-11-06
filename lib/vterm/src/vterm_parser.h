// The parser of the bytes that a program writes to a terminal. It runs the state machine of the escape sequences of the
// DEC terminals, as Paul Williams describes it for the VT500, over UTF-8 text. It returns one action at a time.

#pragma once

#include "pr/base.h"
#include "pr/raw_array.h"
#include "pr/string_view.h"
#include "pr/utf8.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The states of the parser.
enum {
    VtermParserState_Ground,
    VtermParserState_Escape,
    VtermParserState_EscapeIntermediate,
    VtermParserState_CsiEntry,
    VtermParserState_CsiParam,
    VtermParserState_CsiIntermediate,
    VtermParserState_CsiIgnore,
    VtermParserState_DcsEntry,
    VtermParserState_DcsParam,
    VtermParserState_DcsIntermediate,
    VtermParserState_DcsString,
    VtermParserState_DcsIgnore,
    VtermParserState_OscString,
    VtermParserState_IgnoredString,     // SOS, PM or an ignored DCS, whose content is dropped
    VtermParserState_ApcString,
};
typedef uint8_t VtermParserState;

// What a run of input does.
enum {
    VtermActionKind_None,       // the input ran out in the middle of a sequence
    VtermActionKind_Ascii,      // prints the printable ASCII bytes of `text`
    VtermActionKind_Print,      // prints the code points of `codePoints`, which are past ASCII or replacements
    VtermActionKind_Execute,    // runs the control function `final`
    VtermActionKind_Escape,     // dispatches an escape sequence
    VtermActionKind_Csi,        // dispatches a control sequence
    VtermActionKind_Osc,        // dispatches an operating system command, whose content is `text`
    VtermActionKind_Dcs,        // dispatches a device control string, whose content is `text`
    VtermActionKind_Apc,        // dispatches an application program command, whose content is `text`
};
typedef uint8_t VtermActionKind;

// The most parameters of a sequence, and the largest value of a parameter. Further parameters are dropped, and a larger
// value is clamped.
constexpr uint32_t kVtermParserMaxParams = 32;
constexpr uint32_t kVtermParserMaxParamValue = 65535;

// The value of a parameter that the sequence left empty.
constexpr uint32_t kVtermParamNone = 0xffffffffu;

// The most bytes of an OSC, a DCS or an APC string. The parser drops a longer string whole.
constexpr uint32_t kVtermParserMaxStringBytes = 4u << 20;

// An action, valid until the next call to the parser.
struct VtermAction {
    VtermActionKind kind;

    // The private marker of a control sequence (`?`, `>`, `=` or `<`), or zero. Then the sequence's intermediate bytes
    // and its final byte. An escape sequence has no marker.
    char marker;
    char intermediates[2];
    uint8_t intermediateCount;
    char final;

    // The parameters of a control sequence or a device control string. A parameter that follows a colon is a
    // subparameter of the one before it. Bit `i` of `subparams` marks parameter `i` as a subparameter.
    uint32_t paramCount;
    uint32_t params[kVtermParserMaxParams];
    uint32_t subparams;

    // The code points to print, or the text of the action.
    const uint32_t* PR_NULLABLE codePoints;
    uint32_t codePointCount;
    PrStringView text;

    // The byte that ended a string: BEL, or the `\` of ST. A reply to the string ends the same way.
    uint8_t terminator;
};

// The most code points of one Print action.
constexpr uint32_t kVtermParserMaxPrint = 256;

// The state of a parser across writes.
struct VtermParser {
    VtermParserState state;

    PrUtf8Decoder utf8;         // the UTF-8 sequence in progress

    // The code points of the last Print action.
    uint32_t printed[kVtermParserMaxPrint];

    // The sequence in progress, which becomes the action that dispatches it.
    VtermAction sequence;
    VtermAction immediate;      // the action of a control character or a printed run, which leaves `sequence` intact
    bool paramStarted;          // the current parameter has a digit or a separator
    bool stringOverflowed;      // the string passed kVtermParserMaxStringBytes
    RawArray<char> string;
};

// Frees the parser's storage.
void vtermParserReset(VtermParser* parser);

// Returns the parser to the ground state, and drops a sequence in progress.
void vtermParserClear(VtermParser* parser);

// Parses `text` from `*at` up to the next action, returns the action, and advances `*at` past the bytes that it read.
// Returns an action of kind None if it reads the rest of `text` without completing an action.
VtermAction* vtermParserNext(VtermParser* parser, PrStringView text, size_t* at);

// The parameter `index` of `action`, or `fallback` if it is absent or empty.
uint32_t vtermParam(const VtermAction* action, uint32_t index, uint32_t fallback);

// The parameter `index` of `action`, or `fallback` if it is absent, empty or zero. A count's parameter reads this way.
uint32_t vtermParamCount(const VtermAction* action, uint32_t index, uint32_t fallback);

// Returns true if the parameter `index` of `action` is a subparameter of the one before it.
bool vtermParamIsSub(const VtermAction* action, uint32_t index);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
