// Where text divides, over Unicode's line breaking (pr/unicode_line_break.h) and grapheme clusters
// (pr/unicode_grapheme.h): the opportunities to break lines inside words, and the initial letter that a paragraph may
// set apart.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"
#include "pr/unicode_line_break.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Where lines may break inside words.
enum {
    PrTextWordBreak_Normal,     // at Unicode's opportunities
    PrTextWordBreak_BreakAll,   // also between any two letters of a word

    // Never between the letters of East Asian text, which breaks only if a space or punctuation allows it.
    PrTextWordBreak_KeepAll,

    // As Normal, and a word that fits no line breaks between any two grapheme clusters. Those breaks count when the
    // narrowest width of the text is measured.
    PrTextWordBreak_BreakWord,
};
typedef uint8_t PrTextWordBreak;

// Whether a word that fits no line may break between any two of its grapheme clusters.
enum {
    PrTextOverflowWrap_Normal,      // it may not

    // It may, and the breaks count when the narrowest width of the text is measured.
    PrTextOverflowWrap_Anywhere,

    // It may, and the breaks do not count when the narrowest width of the text is measured.
    PrTextOverflowWrap_BreakWord,
};
typedef uint8_t PrTextOverflowWrap;

// How the text that a code point belongs to breaks inside words.
typedef struct PrTextBreakStyle {
    PrTextWordBreak wordBreak;
    PrTextOverflowWrap overflowWrap;
} PrTextBreakStyle;

// An emergency break: one that a word may take only if no other opportunity lets its line fit.
enum {
    PrTextEmergency_None,

    // A break that counts when the narrowest width of the text is measured.
    PrTextEmergency_Anywhere,

    // A break that does not count when the narrowest width of the text is measured.
    PrTextEmergency_BreakWord,
};
typedef uint8_t PrTextEmergency;

// The text whose opportunities prTextBreaksTailor tailors: its UTF-8 bytes, and its `count` code points, the byte that
// each ends at, and the style of each.
typedef struct PrTextBreakDesc {
    PrStringView text;
    const uint32_t* codePoints;
    const uint32_t* ends;
    const PrTextBreakStyle* styles;
    uint32_t count;
} PrTextBreakDesc;

// Tailors `breaks`, Unicode's opportunity after each code point (prUnicodeLineBreaks), by each word break style.
// BreakAll adds an opportunity between two letters of a word. KeepAll removes the opportunity between two letters if
// either is East Asian. A boundary takes the style that both code points around it share, and Normal if they differ. If
// `emergency` is not null, writes to it the emergency break that each code point's style allows after it. A code point
// allows one only if it ends a grapheme cluster of a word, and another cluster of the word or the white space after the
// word follows it.
void prTextBreaksTailor(const PrTextBreakDesc* desc, PrUnicodeLineBreak* breaks,
                        PrTextEmergency* PR_NULLABLE emergency);

// Finds the initial letter of `text`: the first grapheme cluster of a letter, a number or a symbol, with the
// punctuation before it, the punctuation after it that does not open, and the typographic spaces between them. U+3000
// and word separators after the letter are not included. White space before all of it is skipped. A cluster's category
// is that of its first code point. Writes its byte range to `start` and `end` and returns true, or returns false if
// other content comes first.
bool prTextFirstLetter(PrStringView text, size_t* start, size_t* end);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
