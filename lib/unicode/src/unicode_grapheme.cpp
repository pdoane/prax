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
