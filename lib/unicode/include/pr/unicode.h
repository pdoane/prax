// Unicode character properties.
//
// tools/gen/gen_unicode.py takes the properties from the Unicode Character Database.

#pragma once

#include "pr/base.h"
#include "pr/locale.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Grapheme_Cluster_Break, of UAX #29.
enum {
    PrUnicodeGraphemeBreak_Other,
    PrUnicodeGraphemeBreak_CR,
    PrUnicodeGraphemeBreak_LF,
    PrUnicodeGraphemeBreak_Control,
    PrUnicodeGraphemeBreak_Extend,
    PrUnicodeGraphemeBreak_ZWJ,
    PrUnicodeGraphemeBreak_RegionalIndicator,
    PrUnicodeGraphemeBreak_Prepend,
    PrUnicodeGraphemeBreak_SpacingMark,
    PrUnicodeGraphemeBreak_L,
    PrUnicodeGraphemeBreak_V,
    PrUnicodeGraphemeBreak_T,
    PrUnicodeGraphemeBreak_LV,
    PrUnicodeGraphemeBreak_LVT,
};
typedef uint8_t PrUnicodeGraphemeBreak;

// Indic_Conjunct_Break.
enum {
    PrUnicodeIndicConjunctBreak_None,
    PrUnicodeIndicConjunctBreak_Linker,
    PrUnicodeIndicConjunctBreak_Consonant,
    PrUnicodeIndicConjunctBreak_Extend,
};
typedef uint8_t PrUnicodeIndicConjunctBreak;

// Binary properties: the emoji properties of UTS #51, Lowercase and Uppercase, Default_Ignorable_Code_Point, the marks,
// whose General_Category is Mark, and the properties that case mapping's conditions read: Cased, Case_Ignorable and
// Soft_Dotted. A renderer that does not support a default-ignorable code point shows nothing for it (The Unicode
// Standard, section 5.21).
enum {
    PrUnicodeFlag_Emoji = 1u << 0,
    PrUnicodeFlag_EmojiPresentation = 1u << 1,
    PrUnicodeFlag_EmojiModifier = 1u << 2,
    PrUnicodeFlag_EmojiModifierBase = 1u << 3,
    PrUnicodeFlag_EmojiComponent = 1u << 4,
    PrUnicodeFlag_ExtendedPictographic = 1u << 5,
    PrUnicodeFlag_Lowercase = 1u << 6,
    PrUnicodeFlag_Uppercase = 1u << 7,
    PrUnicodeFlag_DefaultIgnorable = 1u << 8,
    PrUnicodeFlag_Mark = 1u << 9,
    PrUnicodeFlag_Cased = 1u << 10,
    PrUnicodeFlag_CaseIgnorable = 1u << 11,
    PrUnicodeFlag_SoftDotted = 1u << 12,
};
typedef uint16_t PrUnicodeFlags;

// Joining_Type: which sides of a character join, in a cursive script (The Unicode Standard, section 9.2).
enum {
    PrUnicodeJoiningType_NonJoining,
    PrUnicodeJoiningType_Right,          // joins the character before it, in logical order
    PrUnicodeJoiningType_Left,           // joins the character after it
    PrUnicodeJoiningType_Dual,           // both
    PrUnicodeJoiningType_JoinCausing,    // makes the characters beside it join, such as a tatweel or ZWJ
    PrUnicodeJoiningType_Transparent,    // skipped, such as a mark
};
typedef uint8_t PrUnicodeJoiningType;

// Vertical_Orientation, of UAX #50: how a character is set in vertical text by default.
enum {
    PrUnicodeVerticalOrientation_Upright,            // U: upright, as in horizontal text
    PrUnicodeVerticalOrientation_Rotated,            // R: turned a quarter clockwise, as in horizontal text
    PrUnicodeVerticalOrientation_TransformedUpright, // Tu: upright, with a vertical alternate if the font has one
    PrUnicodeVerticalOrientation_TransformedRotated, // Tr: a vertical alternate if the font has one, else turned as R
};
typedef uint8_t PrUnicodeVerticalOrientation;

// General_Category, by major class: letters, marks, numbers, punctuation, symbols, separators, and other code points.
// Each member's comment is the value's short name.
enum {
    PrUnicodeGeneralCategory_UppercaseLetter,        // Lu
    PrUnicodeGeneralCategory_LowercaseLetter,        // Ll
    PrUnicodeGeneralCategory_TitlecaseLetter,        // Lt
    PrUnicodeGeneralCategory_ModifierLetter,         // Lm
    PrUnicodeGeneralCategory_OtherLetter,            // Lo
    PrUnicodeGeneralCategory_NonspacingMark,         // Mn
    PrUnicodeGeneralCategory_SpacingMark,            // Mc
    PrUnicodeGeneralCategory_EnclosingMark,          // Me
    PrUnicodeGeneralCategory_DecimalNumber,          // Nd
    PrUnicodeGeneralCategory_LetterNumber,           // Nl
    PrUnicodeGeneralCategory_OtherNumber,            // No
    PrUnicodeGeneralCategory_ConnectorPunctuation,   // Pc
    PrUnicodeGeneralCategory_DashPunctuation,        // Pd
    PrUnicodeGeneralCategory_OpenPunctuation,        // Ps
    PrUnicodeGeneralCategory_ClosePunctuation,       // Pe
    PrUnicodeGeneralCategory_InitialPunctuation,     // Pi
    PrUnicodeGeneralCategory_FinalPunctuation,       // Pf
    PrUnicodeGeneralCategory_OtherPunctuation,       // Po
    PrUnicodeGeneralCategory_MathSymbol,             // Sm
    PrUnicodeGeneralCategory_CurrencySymbol,         // Sc
    PrUnicodeGeneralCategory_ModifierSymbol,         // Sk
    PrUnicodeGeneralCategory_OtherSymbol,            // So
    PrUnicodeGeneralCategory_SpaceSeparator,         // Zs
    PrUnicodeGeneralCategory_LineSeparator,          // Zl
    PrUnicodeGeneralCategory_ParagraphSeparator,     // Zp
    PrUnicodeGeneralCategory_Control,                // Cc
    PrUnicodeGeneralCategory_Format,                 // Cf
    PrUnicodeGeneralCategory_Surrogate,              // Cs
    PrUnicodeGeneralCategory_PrivateUse,             // Co
    PrUnicodeGeneralCategory_Unassigned,             // Cn
};
typedef uint8_t PrUnicodeGeneralCategory;

// Return the Grapheme_Cluster_Break, Indic_Conjunct_Break and binary properties of `codePoint`.
PrUnicodeGraphemeBreak prUnicodeGraphemeBreak(uint32_t codePoint);
PrUnicodeIndicConjunctBreak prUnicodeIndicConjunctBreak(uint32_t codePoint);
PrUnicodeFlags prUnicodeFlags(uint32_t codePoint);

// Returns the Script of `codePoint` by its ISO 15924 code: Common is Zyyy, Inherited is Zinh and Unknown is Zzzz.
PrScript prUnicodeScript(uint32_t codePoint);

// Returns the Joining_Type of `codePoint`.
PrUnicodeJoiningType prUnicodeJoiningType(uint32_t codePoint);

// Returns the Vertical_Orientation of `codePoint`.
PrUnicodeVerticalOrientation prUnicodeVerticalOrientation(uint32_t codePoint);

// Returns the Canonical_Combining_Class of `codePoint`, which is zero for a starter.
uint8_t prUnicodeCombiningClass(uint32_t codePoint);

// Returns the General_Category of `codePoint`. A code point past U+10FFFF is Unassigned.
PrUnicodeGeneralCategory prUnicodeGeneralCategory(uint32_t codePoint);

// Writes the unconditional full uppercase mapping of `codePoint` to `out`, and returns its length in code points. A
// code point with no mapping maps to itself.
uint32_t prUnicodeUppercase(uint32_t codePoint, uint32_t out[PR_NONNULL 3]);

// Writes the unconditional full lowercase mapping of `codePoint` to `out`, and returns its length in code points. A
// code point with no mapping maps to itself.
uint32_t prUnicodeLowercase(uint32_t codePoint, uint32_t out[PR_NONNULL 3]);

// Writes the unconditional full titlecase mapping of `codePoint` to `out`, and returns its length in code points. A
// code point with no mapping maps to itself.
uint32_t prUnicodeTitlecase(uint32_t codePoint, uint32_t out[PR_NONNULL 3]);

// The cases that text maps to (The Unicode Standard, section 3.13).
enum {
    PrUnicodeCase_Lower,
    PrUnicodeCase_Upper,
    PrUnicodeCase_Title,
};
typedef uint8_t PrUnicodeCase;

// The most code points that the case mapping of one code point gives.
enum { kPrUnicodeCaseMaxCodePoints = 3 };

// Writes the full case mapping to `target` of the code point at byte `at` of `text`, UTF-8, to `out`, and returns its
// length in code points, which is zero if the mapping removes the code point. The mapping is the default case
// conversion with the conditional mappings of SpecialCasing.txt: Final_Sigma for every language, and the tailorings of
// `language` for Lithuanian, Turkish and Azerbaijani. The rest of `text` is the context that the conditions read.
// Titlecase maps only the code point itself; the caller decides which code point starts a word.
uint32_t prUnicodeMapCase(PrStringView text, size_t at, PrUnicodeCase target, PrLanguage language,
    uint32_t out[PR_NONNULL kPrUnicodeCaseMaxCodePoints]);

// Writes the full case folding of `codePoint` to `out`, and returns its length in code points. The folding is the
// mappings of CaseFolding.txt with status C or F, without the Turkic mappings. A code point with no folding folds to
// itself. Two strings are equal without regard to case if their foldings are equal.
uint32_t prUnicodeFold(uint32_t codePoint, uint32_t out[PR_NONNULL 3]);

// Returns the full-width form of `codePoint`: the character whose <wide> compatibility decomposition is `codePoint`, or
// the <narrow> decomposition of a half-width form. A code point with no full-width form maps to itself.
uint32_t prUnicodeFullWidth(uint32_t codePoint);

// Returns the full-size form of a small kana, as named without SMALL. Any other code point maps to itself.
uint32_t prUnicodeFullSizeKana(uint32_t codePoint);

// Returns the Mathematical Italic form of `codePoint`, whose <font> decomposition is `codePoint`, or itself if it has
// none. Italic h is U+210E PLANCK CONSTANT.
uint32_t prUnicodeMathItalic(uint32_t codePoint);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
