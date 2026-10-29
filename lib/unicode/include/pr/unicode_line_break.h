// The Unicode Line Breaking Algorithm (https://www.unicode.org/reports/tr14/), untailored: where a text may, must or
// must not break into lines. The classes and properties come from the Unicode Character Database, through
// tools/gen/gen_unicode.py.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Line_Break, by its short names.
enum {
    PrUnicodeLineBreakClass_BK,      // mandatory break
    PrUnicodeLineBreakClass_CR,
    PrUnicodeLineBreakClass_LF,
    PrUnicodeLineBreakClass_NL,      // next line
    PrUnicodeLineBreakClass_SP,
    PrUnicodeLineBreakClass_ZW,      // zero width space
    PrUnicodeLineBreakClass_ZWJ,
    PrUnicodeLineBreakClass_CM,      // combining mark
    PrUnicodeLineBreakClass_WJ,      // word joiner
    PrUnicodeLineBreakClass_GL,      // non-breaking
    PrUnicodeLineBreakClass_BA,      // break after
    PrUnicodeLineBreakClass_BB,      // break before
    PrUnicodeLineBreakClass_B2,      // break on either side, not between
    PrUnicodeLineBreakClass_HY,      // hyphen-minus
    PrUnicodeLineBreakClass_HH,      // unambiguous hyphen
    PrUnicodeLineBreakClass_CB,      // contingent break
    PrUnicodeLineBreakClass_CL,      // close punctuation
    PrUnicodeLineBreakClass_CP,      // close parenthesis
    PrUnicodeLineBreakClass_EX,      // exclamation or interrogation
    PrUnicodeLineBreakClass_IN,      // inseparable
    PrUnicodeLineBreakClass_NS,      // nonstarter
    PrUnicodeLineBreakClass_OP,      // open punctuation
    PrUnicodeLineBreakClass_QU,      // quotation
    PrUnicodeLineBreakClass_IS,      // infix numeric separator
    PrUnicodeLineBreakClass_NU,      // numeric
    PrUnicodeLineBreakClass_PO,      // postfix numeric
    PrUnicodeLineBreakClass_PR,      // prefix numeric
    PrUnicodeLineBreakClass_SY,      // symbols allowing a break after
    PrUnicodeLineBreakClass_AI,      // ambiguous alphabetic or ideographic
    PrUnicodeLineBreakClass_AK,      // aksara
    PrUnicodeLineBreakClass_AL,      // alphabetic
    PrUnicodeLineBreakClass_AP,      // aksara pre-base
    PrUnicodeLineBreakClass_AS,      // aksara start
    PrUnicodeLineBreakClass_CJ,      // conditional Japanese starter
    PrUnicodeLineBreakClass_EB,      // emoji base
    PrUnicodeLineBreakClass_EM,      // emoji modifier
    PrUnicodeLineBreakClass_H2,      // Hangul LV syllable
    PrUnicodeLineBreakClass_H3,      // Hangul LVT syllable
    PrUnicodeLineBreakClass_HL,      // Hebrew letter
    PrUnicodeLineBreakClass_ID,      // ideographic
    PrUnicodeLineBreakClass_JL,      // Hangul L jamo
    PrUnicodeLineBreakClass_JV,      // Hangul V jamo
    PrUnicodeLineBreakClass_JT,      // Hangul T jamo
    PrUnicodeLineBreakClass_RI,      // regional indicator
    PrUnicodeLineBreakClass_SA,      // complex context dependent (South East Asian)
    PrUnicodeLineBreakClass_SG,      // surrogate
    PrUnicodeLineBreakClass_VF,      // virama final
    PrUnicodeLineBreakClass_VI,      // virama
    PrUnicodeLineBreakClass_XX,      // unknown
};
typedef uint8_t PrUnicodeLineBreakClass;

// The properties other than the class that the rules read.
enum {
    PrUnicodeLineBreakFlag_InitialQuote = 1u << 0,           // General_Category Pi
    PrUnicodeLineBreakFlag_FinalQuote = 1u << 1,             // Pf
    PrUnicodeLineBreakFlag_EastAsian = 1u << 2,              // East_Asian_Width F, W or H
    PrUnicodeLineBreakFlag_Mark = 1u << 3,                   // General_Category Mn or Mc
    PrUnicodeLineBreakFlag_UnassignedPictographic = 1u << 4, // Extended_Pictographic and Cn
};
typedef uint8_t PrUnicodeLineBreakFlags;

// Returns the Line_Break class of `codePoint`, and writes its other properties to `*flags`.
PrUnicodeLineBreakClass prUnicodeLineBreakClass(uint32_t codePoint, PrUnicodeLineBreakFlags* flags);

// The break opportunity between a character and the next.
enum {
    PrUnicodeLineBreak_Prohibited,
    PrUnicodeLineBreak_Allowed,
    PrUnicodeLineBreak_Mandatory,
};
typedef uint8_t PrUnicodeLineBreak;

// Computes where the `count` code points may break into lines. `breaks[i]` is the opportunity after code point `i`, and
// the last is always Mandatory (LB3). AI, SG, XX, and SA other than marks resolve to AL. SA marks resolve to CM, and CJ
// to NS (LB1). CB breaks on both sides (LB20). The caller tailors these.
void prUnicodeLineBreaks(const uint32_t* codePoints, uint32_t count, PrUnicodeLineBreak* breaks);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
