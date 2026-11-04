// The Unicode Bidirectional Algorithm (https://www.unicode.org/reports/tr9/): the embedding level of each character in
// a paragraph, and the visual order of the characters of a line. The classes and paired brackets come from the Unicode
// Character Database, through tools/gen/gen_unicode.py.
//
// Mirroring (L4) belongs to shaping. Showing the controls, and joining marks to their base (L3), are left to the
// caller.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Bidi_Class, by its short names.
enum {
    PrUnicodeBidiClass_L,        // left-to-right
    PrUnicodeBidiClass_R,        // right-to-left
    PrUnicodeBidiClass_AL,       // Arabic letter
    PrUnicodeBidiClass_EN,       // European number
    PrUnicodeBidiClass_ES,       // European separator
    PrUnicodeBidiClass_ET,       // European terminator
    PrUnicodeBidiClass_AN,       // Arabic number
    PrUnicodeBidiClass_CS,       // common separator
    PrUnicodeBidiClass_NSM,      // nonspacing mark
    PrUnicodeBidiClass_BN,       // boundary neutral
    PrUnicodeBidiClass_B,        // paragraph separator
    PrUnicodeBidiClass_S,        // segment separator
    PrUnicodeBidiClass_WS,       // white space
    PrUnicodeBidiClass_ON,       // other neutral
    PrUnicodeBidiClass_LRE,      // left-to-right embedding
    PrUnicodeBidiClass_LRO,      // left-to-right override
    PrUnicodeBidiClass_RLE,      // right-to-left embedding
    PrUnicodeBidiClass_RLO,      // right-to-left override
    PrUnicodeBidiClass_PDF,      // pop directional format
    PrUnicodeBidiClass_LRI,      // left-to-right isolate
    PrUnicodeBidiClass_RLI,      // right-to-left isolate
    PrUnicodeBidiClass_FSI,      // first strong isolate
    PrUnicodeBidiClass_PDI,      // pop directional isolate
};
typedef uint8_t PrUnicodeBidiClass;

// The Bidi_Class of `codePoint`.
PrUnicodeBidiClass prUnicodeBidiClass(uint32_t codePoint);

// Bidi_Paired_Bracket_Type.
enum {
    PrUnicodeBidiBracket_None,
    PrUnicodeBidiBracket_Open,
    PrUnicodeBidiBracket_Close,
};
typedef uint8_t PrUnicodeBidiBracket;

// The Bidi_Paired_Bracket_Type of `codePoint`. For a bracket, also writes to `*key` the key that pairs it: the closing
// bracket for an opening one, and the bracket itself for a closing one, each canonically decomposed. An opening and a
// closing bracket pair when their keys are equal.
PrUnicodeBidiBracket prUnicodeBidiBracket(uint32_t codePoint, uint32_t* key);

// Returns the Bidi_Mirroring_Glyph of `codePoint`, the character whose glyph is the mirror image of its glyph, or
// `codePoint` itself if it has none. A right-to-left run shows the mirrored character.
uint32_t prUnicodeBidiMirror(uint32_t codePoint);

// Returns true if the Bidi_Mirrored property of `codePoint` is Yes. A right-to-left run shows such a character
// mirrored, with the glyph of its Bidi_Mirroring_Glyph if it has one.
bool prUnicodeBidiIsMirrored(uint32_t codePoint);

// The strong bidirectional types.
enum {
    PrUnicodeBidiStrong_None,    // a weak or neutral type
    PrUnicodeBidiStrong_Left,    // L
    PrUnicodeBidiStrong_Right,   // R and AL
};
typedef uint8_t PrUnicodeBidiStrong;

// The strong type of `codePoint`.
PrUnicodeBidiStrong prUnicodeBidiStrong(uint32_t codePoint);

// The strong type of the first code point in `text` that has one, or PrUnicodeBidiStrong_None if none does. `text` is
// UTF-8 that may be ill-formed.
PrUnicodeBidiStrong prUnicodeBidiFirstStrong(PrStringView text);

// The level of a character that X9 removes: an embedding, override or pop directional format, or a boundary neutral.
// Reordering keeps such a character with the one before it.
enum { kPrUnicodeBidiRemoved = 0xff };

// The deepest embedding level (BD2).
enum { kPrUnicodeBidiMaxDepth = 125 };

// The base direction of a paragraph.
enum {
    PrUnicodeBidiDirection_LeftToRight,
    PrUnicodeBidiDirection_RightToLeft,
    PrUnicodeBidiDirection_Auto,         // from the first strong character (P2, P3)
};
typedef uint8_t PrUnicodeBidiDirection;

// Resolves the embedding levels of `count` characters of the classes `classes`, by the paragraph rules P2 through I2,
// and writes them to `levels`. Characters that X9 removes get kPrUnicodeBidiRemoved. Brackets pair by their code points
// in `codePoints` (N0), and no brackets pair if `codePoints` is null. A paragraph separator ends a paragraph, and each
// paragraph resolves on its own. Returns the level of the last paragraph.
uint8_t prUnicodeBidiResolve(const PrUnicodeBidiClass* classes, const uint32_t* PR_NULLABLE codePoints,
    uint32_t count, PrUnicodeBidiDirection direction, uint8_t* levels);

// Resets the levels of one line of a paragraph whose level is `paragraphLevel` (L1). `classes` and `levels` cover the
// line. Segment and paragraph separators get the paragraph level, as do the white space and isolates before one of them
// or before the end of the line.
void prUnicodeBidiLineLevels(const PrUnicodeBidiClass* classes, uint32_t count, uint8_t paragraphLevel, uint8_t* levels);

// Writes to `order` the indices of the `count` characters of a line in visual order, left to right, reversing runs by
// level (L2). A removed character stays with the one before it, or at the start of the line.
void prUnicodeBidiReorder(const uint8_t* levels, uint32_t count, uint32_t* order);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
