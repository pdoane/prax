// Where text divides: the word break styles over Unicode's opportunities, and the initial letter.

#include "pr/text_break.h"

#include "pr/unicode.h"
#include "pr/unicode_grapheme.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// Returns true if a code point of class `c` is a letter of a word, which a word break style reads: a letter, a number
// or an ideograph, but not a space, a break, a mark, punctuation or a symbol that UAX #14 classes apart.
bool isLetterUnit(PrUnicodeLineBreakClass c) {
    switch (c) {
        case PrUnicodeLineBreakClass_AL:
        case PrUnicodeLineBreakClass_HL:
        case PrUnicodeLineBreakClass_NU:
        case PrUnicodeLineBreakClass_SA:
        case PrUnicodeLineBreakClass_AI:
        case PrUnicodeLineBreakClass_AK:
        case PrUnicodeLineBreakClass_AS:
        case PrUnicodeLineBreakClass_AP:
        case PrUnicodeLineBreakClass_VF:
        case PrUnicodeLineBreakClass_VI:
        case PrUnicodeLineBreakClass_ID:
        case PrUnicodeLineBreakClass_CJ:
        case PrUnicodeLineBreakClass_H2:
        case PrUnicodeLineBreakClass_H3:
        case PrUnicodeLineBreakClass_JL:
        case PrUnicodeLineBreakClass_JV:
        case PrUnicodeLineBreakClass_JT:
        case PrUnicodeLineBreakClass_XX: return true;
        default: return false;
    }
}

// Returns true if a code point of class `c` is East Asian text, between whose letters KeepAll removes the
// opportunities.
bool isEastAsian(PrUnicodeLineBreakClass c) {
    return c == PrUnicodeLineBreakClass_ID || c == PrUnicodeLineBreakClass_CJ || c == PrUnicodeLineBreakClass_H2
           || c == PrUnicodeLineBreakClass_H3 || c == PrUnicodeLineBreakClass_JL || c == PrUnicodeLineBreakClass_JV
           || c == PrUnicodeLineBreakClass_JT;
}

// Returns true if a code point of class `c` separates words, so that overflow-wrap adds nothing after it: white space
// and the breaks.
bool separates(PrUnicodeLineBreakClass c) {
    return c == PrUnicodeLineBreakClass_SP || c == PrUnicodeLineBreakClass_BK || c == PrUnicodeLineBreakClass_CR
           || c == PrUnicodeLineBreakClass_LF || c == PrUnicodeLineBreakClass_NL || c == PrUnicodeLineBreakClass_ZW;
}

// The classes of grapheme clusters that finding the initial letter reads.
enum {
    LetterClass_Letter,             // L*, N* or S*: a first letter
    LetterClass_Punctuation,        // P* other than Ps and Pd, which may also follow the letter
    LetterClass_Leading,            // Ps or Pd, which only precede it
    LetterClass_Space,              // Zs other than U+3000, which may lie between them
    LetterClass_Separator,          // a word separator, a space that divides words, which only precedes it
    LetterClass_White,              // other white space, which collapses before the text
    LetterClass_Other,
};
typedef uint8_t LetterClass;

LetterClass letterClassOf(uint32_t codePoint) {
    if (codePoint == 0x20 || codePoint == 0xa0 || codePoint == 0x1361 || codePoint == 0x10100 || codePoint == 0x10101
        || codePoint == 0x1039f || codePoint == 0x1091f)
        return LetterClass_Separator;
    if (codePoint == '\t' || codePoint == '\n' || codePoint == '\r' || codePoint == '\f')
        return LetterClass_White;
    switch (prUnicodeGeneralCategory(codePoint)) {
        case PrUnicodeGeneralCategory_UppercaseLetter:
        case PrUnicodeGeneralCategory_LowercaseLetter:
        case PrUnicodeGeneralCategory_TitlecaseLetter:
        case PrUnicodeGeneralCategory_ModifierLetter:
        case PrUnicodeGeneralCategory_OtherLetter:
        case PrUnicodeGeneralCategory_DecimalNumber:
        case PrUnicodeGeneralCategory_LetterNumber:
        case PrUnicodeGeneralCategory_OtherNumber:
        case PrUnicodeGeneralCategory_MathSymbol:
        case PrUnicodeGeneralCategory_CurrencySymbol:
        case PrUnicodeGeneralCategory_ModifierSymbol:
        case PrUnicodeGeneralCategory_OtherSymbol: return LetterClass_Letter;
        case PrUnicodeGeneralCategory_DashPunctuation:
        case PrUnicodeGeneralCategory_OpenPunctuation: return LetterClass_Leading;
        case PrUnicodeGeneralCategory_ConnectorPunctuation:
        case PrUnicodeGeneralCategory_ClosePunctuation:
        case PrUnicodeGeneralCategory_InitialPunctuation:
        case PrUnicodeGeneralCategory_FinalPunctuation:
        case PrUnicodeGeneralCategory_OtherPunctuation: return LetterClass_Punctuation;
        case PrUnicodeGeneralCategory_SpaceSeparator:
            return codePoint == 0x3000 ? LetterClass_Other : LetterClass_Space;
        default: return LetterClass_Other;
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prTextBreaksTailor(const PrTextBreakDesc* desc, PrUnicodeLineBreak* breaks,
                        PrTextEmergency* PR_NULLABLE emergency) {
    const PrTextBreakDesc& d = *desc;
    // The byte at which the grapheme cluster after the code point being read starts. A code point ends its cluster if
    // its end reaches this byte.
    size_t cluster = 0;
    PrUnicodeLineBreakFlags flags = 0;
    PrUnicodeLineBreakClass before =
        d.count != 0 ? prUnicodeLineBreakClass(d.codePoints[0], &flags) : PrUnicodeLineBreakClass_XX;
    for (uint32_t i = 0; i < d.count; ++i) {
        if (emergency != nullptr)
            emergency[i] = PrTextEmergency_None;
        size_t start = i == 0 ? 0 : d.ends[i - 1];
        if (start >= cluster)
            cluster = prUnicodeGraphemeNext(d.text, start);
        if (i + 1 == d.count)
            break;
        PrUnicodeLineBreakClass after = prUnicodeLineBreakClass(d.codePoints[i + 1], &flags);
        bool ends = d.ends[i] >= cluster;   // the code point ends its cluster
        const PrTextBreakStyle& own = d.styles[i];
        const PrTextBreakStyle& next = d.styles[i + 1];
        PrTextWordBreak shared = own.wordBreak == next.wordBreak ? own.wordBreak
                                                                 : PrTextWordBreak(PrTextWordBreak_Normal);
        bool letters = isLetterUnit(before) && isLetterUnit(after);
        if (shared == PrTextWordBreak_BreakAll && ends && letters && breaks[i] == PrUnicodeLineBreak_Prohibited)
            breaks[i] = PrUnicodeLineBreak_Allowed;
        else if (shared == PrTextWordBreak_KeepAll && letters && (isEastAsian(before) || isEastAsian(after))
                 && breaks[i] == PrUnicodeLineBreak_Allowed)
            breaks[i] = PrUnicodeLineBreak_Prohibited;
        // A word may also break before the white space after it, which a line keeps if its spaces do not hang.
        if (emergency != nullptr && ends && breaks[i] == PrUnicodeLineBreak_Prohibited && !separates(before)) {
            if (own.overflowWrap == PrTextOverflowWrap_Anywhere || own.wordBreak == PrTextWordBreak_BreakWord)
                emergency[i] = PrTextEmergency_Anywhere;
            else if (own.overflowWrap == PrTextOverflowWrap_BreakWord)
                emergency[i] = PrTextEmergency_BreakWord;
        }
        // A combining mark takes the class of the character it combines with (UAX14 LB9).
        if (after != PrUnicodeLineBreakClass_CM && after != PrUnicodeLineBreakClass_ZWJ)
            before = after;
    }
}

bool prTextFirstLetter(PrStringView text, size_t* start, size_t* end) {
    auto classAt = [&](size_t at) {
        uint32_t codePoint = 0;
        prUtf8Decode(text, at, &codePoint);
        return letterClassOf(codePoint);
    };
    size_t at = 0;
    while (at < text.size && (classAt(at) == LetterClass_Space || classAt(at) == LetterClass_Separator
                              || classAt(at) == LetterClass_White))
        at = prUnicodeGraphemeNext(text, at);
    *start = at;
    // Punctuation, and the spaces after it, before the letter.
    if (at < text.size && (classAt(at) == LetterClass_Punctuation || classAt(at) == LetterClass_Leading)) {
        while (at < text.size && classAt(at) != LetterClass_Letter && classAt(at) != LetterClass_Other
               && classAt(at) != LetterClass_White)
            at = prUnicodeGraphemeNext(text, at);
    }
    if (at >= text.size || classAt(at) != LetterClass_Letter)
        return false;
    at = prUnicodeGraphemeNext(text, at);
    *end = at;
    // Punctuation after the letter, with the spaces between that are not word separators.
    while (at < text.size) {
        LetterClass c = classAt(at);
        if (c != LetterClass_Punctuation && c != LetterClass_Space)
            break;
        at = prUnicodeGraphemeNext(text, at);
        if (c == LetterClass_Punctuation)
            *end = at;
    }
    return true;
}
