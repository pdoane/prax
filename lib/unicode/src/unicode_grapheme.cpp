// Grapheme clusters.
//
// The rules and their numbering are those of UAX #29. They are applied between each code point and the next, using
// state left by earlier code points of the cluster.

#include "pr/unicode_grapheme.h"

#include "pr/unicode.h"
#include "pr/utf8.h"

namespace pr_impl {
namespace {

// State from earlier code points, for the rules that look back further than one code point.
struct Look {
    uint32_t regional;          // the number of consecutive regional indicators
    bool pictographic;          // true after Extended_Pictographic Extend*
    bool pictographicZwj;       // true after Extended_Pictographic Extend* ZWJ
    uint8_t conjunct;           // 1 after a Consonant [Extend]*, 2 once a Linker follows
};

bool isControl(PrUnicodeGraphemeBreak b) {
    return b == PrUnicodeGraphemeBreak_Control || b == PrUnicodeGraphemeBreak_CR || b == PrUnicodeGraphemeBreak_LF;
}

// Returns true if the rules keep `next` in the same cluster as `before`.
bool keeps(PrUnicodeGraphemeBreak before, PrUnicodeGraphemeBreak next, uint32_t nextCodePoint, const Look& look) {
    if (before == PrUnicodeGraphemeBreak_CR && next == PrUnicodeGraphemeBreak_LF)
        return true;                                                // GB3
    if (isControl(before) || isControl(next))
        return false;                                               // GB4, GB5
    if (before == PrUnicodeGraphemeBreak_L &&
            (next == PrUnicodeGraphemeBreak_L || next == PrUnicodeGraphemeBreak_V || next == PrUnicodeGraphemeBreak_LV ||
                next == PrUnicodeGraphemeBreak_LVT))
        return true;                                                // GB6
    if ((before == PrUnicodeGraphemeBreak_LV || before == PrUnicodeGraphemeBreak_V) &&
            (next == PrUnicodeGraphemeBreak_V || next == PrUnicodeGraphemeBreak_T))
        return true;                                                // GB7
    if ((before == PrUnicodeGraphemeBreak_LVT || before == PrUnicodeGraphemeBreak_T) &&
            next == PrUnicodeGraphemeBreak_T)
        return true;                                                // GB8
    if (next == PrUnicodeGraphemeBreak_Extend || next == PrUnicodeGraphemeBreak_ZWJ)
        return true;                                                // GB9
    if (next == PrUnicodeGraphemeBreak_SpacingMark)
        return true;                                                // GB9a
    if (before == PrUnicodeGraphemeBreak_Prepend)
        return true;                                                // GB9b
    if (look.conjunct == 2 && prUnicodeIndicConjunctBreak(nextCodePoint) == PrUnicodeIndicConjunctBreak_Consonant)
        return true;                                                // GB9c
    if (look.pictographicZwj && (prUnicodeFlags(nextCodePoint) & PrUnicodeFlag_ExtendedPictographic))
        return true;                                                // GB11
    if (before == PrUnicodeGraphemeBreak_RegionalIndicator && next == PrUnicodeGraphemeBreak_RegionalIndicator)
        return look.regional % 2 == 1;                              // GB12, GB13
    return false;                                                   // GB999
}

void advance(Look& look, uint32_t codePoint, PrUnicodeGraphemeBreak b) {
    look.regional = b == PrUnicodeGraphemeBreak_RegionalIndicator ? look.regional + 1 : 0;
    bool pictographic = (prUnicodeFlags(codePoint) & PrUnicodeFlag_ExtendedPictographic) != 0;
    look.pictographicZwj = look.pictographic && b == PrUnicodeGraphemeBreak_ZWJ;
    look.pictographic = pictographic || (look.pictographic && b == PrUnicodeGraphemeBreak_Extend);
    PrUnicodeIndicConjunctBreak incb = prUnicodeIndicConjunctBreak(codePoint);
    if (incb == PrUnicodeIndicConjunctBreak_Consonant)
        look.conjunct = 1;
    else if (incb == PrUnicodeIndicConjunctBreak_Linker && look.conjunct != 0)
        look.conjunct = 2;
    else if (incb != PrUnicodeIndicConjunctBreak_Extend)
        look.conjunct = 0;
}

bool isLetter(PrUnicodeWordBreak b) {
    return b == PrUnicodeWordBreak_ALetter || b == PrUnicodeWordBreak_HebrewLetter;
}

bool isMidLetter(PrUnicodeWordBreak b) {
    return b == PrUnicodeWordBreak_MidLetter || b == PrUnicodeWordBreak_MidNumLet ||
        b == PrUnicodeWordBreak_SingleQuote;
}

bool isMidNumber(PrUnicodeWordBreak b) {
    return b == PrUnicodeWordBreak_MidNum || b == PrUnicodeWordBreak_MidNumLet || b == PrUnicodeWordBreak_SingleQuote;
}

bool isIgnored(PrUnicodeWordBreak b) {
    return b == PrUnicodeWordBreak_Extend || b == PrUnicodeWordBreak_Format || b == PrUnicodeWordBreak_ZWJ;
}

bool isNewline(PrUnicodeWordBreak b) {
    return b == PrUnicodeWordBreak_CR || b == PrUnicodeWordBreak_LF || b == PrUnicodeWordBreak_Newline;
}

// Returns the class of the first code point at or after `at` that WB4 does not ignore, or Other at the end of the text.
PrUnicodeWordBreak wordClassAfter(PrStringView text, size_t at) {
    while (at < text.size) {
        uint32_t codePoint = 0;
        at += prUtf8Decode(text, at, &codePoint);
        PrUnicodeWordBreak b = prUnicodeWordBreak(codePoint);
        if (!isIgnored(b))
            return b;
    }
    return PrUnicodeWordBreak_Other;
}

// Returns true if the rules keep `next` in the same segment as `before`, which follows `earlier`. `after` is the class
// that follows `next`. WB4 has removed what it ignores from all of them.
bool wordKeeps(PrUnicodeWordBreak earlier, PrUnicodeWordBreak before, PrUnicodeWordBreak next, PrUnicodeWordBreak after,
        uint32_t regional) {
    typedef PrUnicodeWordBreak B;
    const B hebrew = PrUnicodeWordBreak_HebrewLetter;
    const B number = PrUnicodeWordBreak_Numeric;
    const B katakana = PrUnicodeWordBreak_Katakana;
    const B connector = PrUnicodeWordBreak_ExtendNumLet;
    if (isLetter(before) && isLetter(next))
        return true;                                                // WB5
    if (isLetter(before) && isMidLetter(next) && isLetter(after))
        return true;                                                // WB6
    if (isLetter(earlier) && isMidLetter(before) && isLetter(next))
        return true;                                                // WB7
    if (before == hebrew && next == PrUnicodeWordBreak_SingleQuote)
        return true;                                                // WB7a
    if (before == hebrew && next == PrUnicodeWordBreak_DoubleQuote && after == hebrew)
        return true;                                                // WB7b
    if (earlier == hebrew && before == PrUnicodeWordBreak_DoubleQuote && next == hebrew)
        return true;                                                // WB7c
    if ((before == number || isLetter(before)) && next == number)
        return true;                                                // WB8, WB9
    if (before == number && isLetter(next))
        return true;                                                // WB10
    if (earlier == number && isMidNumber(before) && next == number)
        return true;                                                // WB11
    if (before == number && isMidNumber(next) && after == number)
        return true;                                                // WB12
    if (before == katakana && next == katakana)
        return true;                                                // WB13
    if ((isLetter(before) || before == number || before == katakana || before == connector) && next == connector)
        return true;                                                // WB13a
    if (before == connector && (isLetter(next) || next == number || next == katakana))
        return true;                                                // WB13b
    if (before == PrUnicodeWordBreak_RegionalIndicator && next == before && regional % 2 == 1)
        return true;                                                // WB15, WB16
    return false;                                                   // WB999
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

size_t prUnicodeGraphemeNext(PrStringView text, size_t at) {
    if (at >= text.size)
        return text.size;
    uint32_t codePoint = 0;
    at += prUtf8Decode(text, at, &codePoint);
    PrUnicodeGraphemeBreak before = prUnicodeGraphemeBreak(codePoint);
    Look look{};
    advance(look, codePoint, before);
    while (at < text.size) {
        uint32_t next = 0;
        uint32_t length = prUtf8Decode(text, at, &next);
        PrUnicodeGraphemeBreak b = prUnicodeGraphemeBreak(next);
        if (!keeps(before, b, next, look))
            break;
        advance(look, next, b);
        before = b;
        at += length;
    }
    return at;
}

size_t prUnicodeWordNext(PrStringView text, size_t at) {
    if (at >= text.size)
        return text.size;
    uint32_t codePoint = 0;
    at += prUtf8Decode(text, at, &codePoint);
    PrUnicodeWordBreak raw = prUnicodeWordBreak(codePoint);
    if (raw == PrUnicodeWordBreak_CR && at < text.size && text.data[at] == '\n')
        return at + 1;                                              // WB3
    if (isNewline(raw))
        return at;                                                  // WB3a
    PrUnicodeWordBreak before = raw;
    PrUnicodeWordBreak earlier = PrUnicodeWordBreak_Other;
    uint32_t regional = raw == PrUnicodeWordBreak_RegionalIndicator ? 1 : 0;
    while (at < text.size) {
        uint32_t next = 0;
        uint32_t length = prUtf8Decode(text, at, &next);
        PrUnicodeWordBreak b = prUnicodeWordBreak(next);
        if (isNewline(b))
            break;                                                  // WB3b
        bool joined = raw == PrUnicodeWordBreak_ZWJ && (prUnicodeFlags(next) & PrUnicodeFlag_ExtendedPictographic) != 0;
        bool spaces = raw == PrUnicodeWordBreak_WSegSpace && b == PrUnicodeWordBreak_WSegSpace;
        if (isIgnored(b)) {
            raw = b;                                                // WB4
            at += length;
            continue;
        }
        if (!joined && !spaces &&                                   // WB3c, WB3d
                !wordKeeps(earlier, before, b, wordClassAfter(text, at + length), regional))
            break;
        regional = b == PrUnicodeWordBreak_RegionalIndicator ? regional + 1 : 0;
        earlier = before;
        before = b;
        raw = b;
        at += length;
    }
    return at;
}
