// The Unicode Line Breaking Algorithm, with the rule numbers of UAX #14. line_break_data.cpp holds the classes and
// properties it reads.
//
// The rules run over the text with each combining sequence treated as its base (LB9). A rule's neighbors are therefore
// the bases around a position, and a position inside a sequence never breaks.

#include "pr/unicode_line_break.h"

#include "pr/array.h"

namespace pr_impl {

namespace {

// A character as the rules after LB9 see it: a base, with its combining marks.
struct Base {
    uint32_t index;         // the index of its code point
    uint32_t last;          // the index of the last code point of its sequence
    PrUnicodeLineBreakClass c;
    PrUnicodeLineBreakFlags flags;
    bool dottedCircle;      // true if the base is U+25CC, which LB28a names
};

// Returns the class that LB1 resolves a character to.
PrUnicodeLineBreakClass resolved(PrUnicodeLineBreakClass c, PrUnicodeLineBreakFlags flags) {
    switch (c) {
        case PrUnicodeLineBreakClass_AI:
        case PrUnicodeLineBreakClass_SG:
        case PrUnicodeLineBreakClass_XX: return PrUnicodeLineBreakClass_AL;
        case PrUnicodeLineBreakClass_SA:
            return (flags & PrUnicodeLineBreakFlag_Mark) != 0 ? PrUnicodeLineBreakClass_CM : PrUnicodeLineBreakClass_AL;
        case PrUnicodeLineBreakClass_CJ: return PrUnicodeLineBreakClass_NS;
        default: return c;
    }
}

bool isHardBreak(PrUnicodeLineBreakClass c) {
    return c == PrUnicodeLineBreakClass_BK || c == PrUnicodeLineBreakClass_CR || c == PrUnicodeLineBreakClass_LF ||
        c == PrUnicodeLineBreakClass_NL;
}

bool isAlphabetic(PrUnicodeLineBreakClass c) {
    return c == PrUnicodeLineBreakClass_AL || c == PrUnicodeLineBreakClass_HL;
}

bool isHangul(PrUnicodeLineBreakClass c) {
    return c == PrUnicodeLineBreakClass_JL || c == PrUnicodeLineBreakClass_JV || c == PrUnicodeLineBreakClass_JT ||
        c == PrUnicodeLineBreakClass_H2 || c == PrUnicodeLineBreakClass_H3;
}

struct Text {
    const Base* bases;
    uint32_t count;

    PrUnicodeLineBreakClass at(uint32_t k) const { return bases[k].c; }
    bool eastAsian(uint32_t k) const {
        return (bases[k].flags & PrUnicodeLineBreakFlag_EastAsian) != 0;
    }
    // Returns the base before k, skipping any spaces, or kArrayMissing if there is none.
    uint32_t beforeSpaces(uint32_t k) const {
        uint32_t j = k;
        while (j > 0 && at(j - 1) == PrUnicodeLineBreakClass_SP)
            --j;
        return j > 0 ? j - 1 : kArrayMissing;
    }
    // Returns the base before k, skipping any SY and IS, or kArrayMissing if there is none.
    uint32_t beforeNumericPunctuation(uint32_t k) const {
        uint32_t j = k;
        while (j > 0 && (at(j - 1) == PrUnicodeLineBreakClass_SY || at(j - 1) == PrUnicodeLineBreakClass_IS))
            --j;
        return j > 0 ? j - 1 : kArrayMissing;
    }
    // Returns true if base k is AK, AS or U+25CC, which are the aksara that LB28a joins.
    bool aksara(uint32_t k) const {
        return at(k) == PrUnicodeLineBreakClass_AK || at(k) == PrUnicodeLineBreakClass_AS || bases[k].dottedCircle;
    }
};

// Returns the break opportunity between bases k - 1 and k, by applying the rules after LB9 in order.
PrUnicodeLineBreak between(const Text& t, uint32_t k, bool afterJoiner) {
    PrUnicodeLineBreakClass a = t.at(k - 1);
    PrUnicodeLineBreakClass b = t.at(k);
    uint32_t n = t.count;
    // LB4, LB5: hard line breaks.
    if (a == PrUnicodeLineBreakClass_BK)
        return PrUnicodeLineBreak_Mandatory;
    if (a == PrUnicodeLineBreakClass_CR && b == PrUnicodeLineBreakClass_LF)
        return PrUnicodeLineBreak_Prohibited;
    if (a == PrUnicodeLineBreakClass_CR || a == PrUnicodeLineBreakClass_LF || a == PrUnicodeLineBreakClass_NL)
        return PrUnicodeLineBreak_Mandatory;
    // LB6, LB7.
    if (isHardBreak(b) || b == PrUnicodeLineBreakClass_SP || b == PrUnicodeLineBreakClass_ZW)
        return PrUnicodeLineBreak_Prohibited;
    // LB8: after a zero width space, and the spaces after it.
    uint32_t j = t.beforeSpaces(k);
    if ((a == PrUnicodeLineBreakClass_ZW || a == PrUnicodeLineBreakClass_SP) && j != kArrayMissing &&
            t.at(j) == PrUnicodeLineBreakClass_ZW)
        return PrUnicodeLineBreak_Allowed;
    if (a == PrUnicodeLineBreakClass_ZW)
        return PrUnicodeLineBreak_Allowed;
    // LB8a: after a zero width joiner.
    if (afterJoiner)
        return PrUnicodeLineBreak_Prohibited;
    // LB11, LB12, LB12a.
    if (a == PrUnicodeLineBreakClass_WJ || b == PrUnicodeLineBreakClass_WJ || a == PrUnicodeLineBreakClass_GL)
        return PrUnicodeLineBreak_Prohibited;
    if (b == PrUnicodeLineBreakClass_GL && a != PrUnicodeLineBreakClass_SP && a != PrUnicodeLineBreakClass_BA &&
            a != PrUnicodeLineBreakClass_HY && a != PrUnicodeLineBreakClass_HH)
        return PrUnicodeLineBreak_Prohibited;
    // LB13, LB14.
    if (b == PrUnicodeLineBreakClass_CL || b == PrUnicodeLineBreakClass_CP || b == PrUnicodeLineBreakClass_EX ||
            b == PrUnicodeLineBreakClass_SY)
        return PrUnicodeLineBreak_Prohibited;
    if (j != kArrayMissing && t.at(j) == PrUnicodeLineBreakClass_OP)
        return PrUnicodeLineBreak_Prohibited;
    // LB15a: after an initial quotation mark that starts something, and spaces after it.
    if (j != kArrayMissing && t.at(j) == PrUnicodeLineBreakClass_QU &&
            (t.bases[j].flags & PrUnicodeLineBreakFlag_InitialQuote) != 0) {
        PrUnicodeLineBreakClass before = j == 0 ? PrUnicodeLineBreakClass_BK : t.at(j - 1);
        if (isHardBreak(before) || before == PrUnicodeLineBreakClass_OP || before == PrUnicodeLineBreakClass_QU ||
                before == PrUnicodeLineBreakClass_GL || before == PrUnicodeLineBreakClass_SP ||
                before == PrUnicodeLineBreakClass_ZW)
            return PrUnicodeLineBreak_Prohibited;
    }
    // LB15b: before a final quotation mark that ends something.
    if (b == PrUnicodeLineBreakClass_QU && (t.bases[k].flags & PrUnicodeLineBreakFlag_FinalQuote) != 0) {
        if (k + 1 == n)
            return PrUnicodeLineBreak_Prohibited;
        PrUnicodeLineBreakClass after = t.at(k + 1);
        if (after == PrUnicodeLineBreakClass_SP || after == PrUnicodeLineBreakClass_GL ||
                after == PrUnicodeLineBreakClass_WJ || after == PrUnicodeLineBreakClass_CL ||
                after == PrUnicodeLineBreakClass_QU || after == PrUnicodeLineBreakClass_CP ||
                after == PrUnicodeLineBreakClass_EX || after == PrUnicodeLineBreakClass_IS ||
                after == PrUnicodeLineBreakClass_SY || isHardBreak(after) || after == PrUnicodeLineBreakClass_ZW)
            return PrUnicodeLineBreak_Prohibited;
    }
    // LB15c, LB15d.
    if (a == PrUnicodeLineBreakClass_SP && b == PrUnicodeLineBreakClass_IS && k + 1 < n &&
            t.at(k + 1) == PrUnicodeLineBreakClass_NU)
        return PrUnicodeLineBreak_Allowed;
    if (b == PrUnicodeLineBreakClass_IS)
        return PrUnicodeLineBreak_Prohibited;
    // LB16, LB17.
    if (b == PrUnicodeLineBreakClass_NS && j != kArrayMissing &&
            (t.at(j) == PrUnicodeLineBreakClass_CL || t.at(j) == PrUnicodeLineBreakClass_CP))
        return PrUnicodeLineBreak_Prohibited;
    if (b == PrUnicodeLineBreakClass_B2 && j != kArrayMissing && t.at(j) == PrUnicodeLineBreakClass_B2)
        return PrUnicodeLineBreak_Prohibited;
    // LB18.
    if (a == PrUnicodeLineBreakClass_SP)
        return PrUnicodeLineBreak_Allowed;
    // LB19: quotation marks that are not initial or final.
    if (b == PrUnicodeLineBreakClass_QU && (t.bases[k].flags & PrUnicodeLineBreakFlag_InitialQuote) == 0)
        return PrUnicodeLineBreak_Prohibited;
    if (a == PrUnicodeLineBreakClass_QU && (t.bases[k - 1].flags & PrUnicodeLineBreakFlag_FinalQuote) == 0)
        return PrUnicodeLineBreak_Prohibited;
    // LB19a: quotation marks, unless East Asian text surrounds them.
    if (b == PrUnicodeLineBreakClass_QU && (!t.eastAsian(k - 1) || k + 1 == n || !t.eastAsian(k + 1)))
        return PrUnicodeLineBreak_Prohibited;
    if (a == PrUnicodeLineBreakClass_QU && (!t.eastAsian(k) || k < 2 || !t.eastAsian(k - 2)))
        return PrUnicodeLineBreak_Prohibited;
    // LB20.
    if (a == PrUnicodeLineBreakClass_CB || b == PrUnicodeLineBreakClass_CB)
        return PrUnicodeLineBreak_Allowed;
    // LB20a: after a word-initial hyphen.
    if ((a == PrUnicodeLineBreakClass_HY || a == PrUnicodeLineBreakClass_HH) && isAlphabetic(b)) {
        PrUnicodeLineBreakClass before = k < 2 ? PrUnicodeLineBreakClass_BK : t.at(k - 2);
        if (isHardBreak(before) || before == PrUnicodeLineBreakClass_SP || before == PrUnicodeLineBreakClass_ZW ||
                before == PrUnicodeLineBreakClass_CB || before == PrUnicodeLineBreakClass_GL)
            return PrUnicodeLineBreak_Prohibited;
    }
    // LB21, LB21a, LB21b, LB22.
    if (b == PrUnicodeLineBreakClass_BA || b == PrUnicodeLineBreakClass_HH || b == PrUnicodeLineBreakClass_HY ||
            b == PrUnicodeLineBreakClass_NS || a == PrUnicodeLineBreakClass_BB)
        return PrUnicodeLineBreak_Prohibited;
    if ((a == PrUnicodeLineBreakClass_HY || a == PrUnicodeLineBreakClass_HH) && b != PrUnicodeLineBreakClass_HL &&
            k >= 2 && t.at(k - 2) == PrUnicodeLineBreakClass_HL)
        return PrUnicodeLineBreak_Prohibited;
    if (a == PrUnicodeLineBreakClass_SY && b == PrUnicodeLineBreakClass_HL)
        return PrUnicodeLineBreak_Prohibited;
    if (b == PrUnicodeLineBreakClass_IN)
        return PrUnicodeLineBreak_Prohibited;
    // LB23, LB23a, LB24: numbers with letters, prefixes and suffixes.
    if ((isAlphabetic(a) && b == PrUnicodeLineBreakClass_NU) || (a == PrUnicodeLineBreakClass_NU && isAlphabetic(b)))
        return PrUnicodeLineBreak_Prohibited;
    bool ideograph = b == PrUnicodeLineBreakClass_ID || b == PrUnicodeLineBreakClass_EB ||
        b == PrUnicodeLineBreakClass_EM;
    if (a == PrUnicodeLineBreakClass_PR && ideograph)
        return PrUnicodeLineBreak_Prohibited;
    if ((a == PrUnicodeLineBreakClass_ID || a == PrUnicodeLineBreakClass_EB || a == PrUnicodeLineBreakClass_EM) &&
            b == PrUnicodeLineBreakClass_PO)
        return PrUnicodeLineBreak_Prohibited;
    if (((a == PrUnicodeLineBreakClass_PR || a == PrUnicodeLineBreakClass_PO) && isAlphabetic(b)) ||
            (isAlphabetic(a) && (b == PrUnicodeLineBreakClass_PR || b == PrUnicodeLineBreakClass_PO)))
        return PrUnicodeLineBreak_Prohibited;
    // LB25: numbers.
    if (b == PrUnicodeLineBreakClass_PO || b == PrUnicodeLineBreakClass_PR) {
        uint32_t number;
        if (a == PrUnicodeLineBreakClass_CL || a == PrUnicodeLineBreakClass_CP)
            number = (k >= 2 ? t.beforeNumericPunctuation(k - 1) : kArrayMissing);
        else
            number = t.beforeNumericPunctuation(k);
        if (number != kArrayMissing && t.at(number) == PrUnicodeLineBreakClass_NU)
            return PrUnicodeLineBreak_Prohibited;
    }
    if (a == PrUnicodeLineBreakClass_PO || a == PrUnicodeLineBreakClass_PR) {
        if (b == PrUnicodeLineBreakClass_NU)
            return PrUnicodeLineBreak_Prohibited;
        if (b == PrUnicodeLineBreakClass_OP && k + 1 < n && (t.at(k + 1) == PrUnicodeLineBreakClass_NU ||
                (t.at(k + 1) == PrUnicodeLineBreakClass_IS && k + 2 < n && t.at(k + 2) == PrUnicodeLineBreakClass_NU)))
            return PrUnicodeLineBreak_Prohibited;
    }
    if ((a == PrUnicodeLineBreakClass_HY || a == PrUnicodeLineBreakClass_IS) && b == PrUnicodeLineBreakClass_NU)
        return PrUnicodeLineBreak_Prohibited;
    if (b == PrUnicodeLineBreakClass_NU) {
        uint32_t number = t.beforeNumericPunctuation(k);
        if (number != kArrayMissing && t.at(number) == PrUnicodeLineBreakClass_NU)
            return PrUnicodeLineBreak_Prohibited;
    }
    // LB26, LB27: Korean syllable blocks.
    if (a == PrUnicodeLineBreakClass_JL &&
            (b == PrUnicodeLineBreakClass_JL || b == PrUnicodeLineBreakClass_JV || b == PrUnicodeLineBreakClass_H2 ||
                b == PrUnicodeLineBreakClass_H3))
        return PrUnicodeLineBreak_Prohibited;
    if ((a == PrUnicodeLineBreakClass_JV || a == PrUnicodeLineBreakClass_H2) &&
            (b == PrUnicodeLineBreakClass_JV || b == PrUnicodeLineBreakClass_JT))
        return PrUnicodeLineBreak_Prohibited;
    if ((a == PrUnicodeLineBreakClass_JT || a == PrUnicodeLineBreakClass_H3) && b == PrUnicodeLineBreakClass_JT)
        return PrUnicodeLineBreak_Prohibited;
    if ((isHangul(a) && b == PrUnicodeLineBreakClass_PO) || (a == PrUnicodeLineBreakClass_PR && isHangul(b)))
        return PrUnicodeLineBreak_Prohibited;
    // LB28.
    if (isAlphabetic(a) && isAlphabetic(b))
        return PrUnicodeLineBreak_Prohibited;
    // LB28a: Brahmic orthographic syllables.
    if (a == PrUnicodeLineBreakClass_AP && t.aksara(k))
        return PrUnicodeLineBreak_Prohibited;
    if (t.aksara(k - 1) && (b == PrUnicodeLineBreakClass_VF || b == PrUnicodeLineBreakClass_VI))
        return PrUnicodeLineBreak_Prohibited;
    if (a == PrUnicodeLineBreakClass_VI && k >= 2 && t.aksara(k - 2) &&
            (b == PrUnicodeLineBreakClass_AK || t.bases[k].dottedCircle))
        return PrUnicodeLineBreak_Prohibited;
    if (t.aksara(k - 1) && t.aksara(k) && k + 1 < n && t.at(k + 1) == PrUnicodeLineBreakClass_VF)
        return PrUnicodeLineBreak_Prohibited;
    // LB29, LB30.
    if (a == PrUnicodeLineBreakClass_IS && isAlphabetic(b))
        return PrUnicodeLineBreak_Prohibited;
    if ((isAlphabetic(a) || a == PrUnicodeLineBreakClass_NU) && b == PrUnicodeLineBreakClass_OP && !t.eastAsian(k))
        return PrUnicodeLineBreak_Prohibited;
    if (a == PrUnicodeLineBreakClass_CP && !t.eastAsian(k - 1) && (isAlphabetic(b) || b == PrUnicodeLineBreakClass_NU))
        return PrUnicodeLineBreak_Prohibited;
    // LB30a: regional indicators in pairs.
    if (a == PrUnicodeLineBreakClass_RI && b == PrUnicodeLineBreakClass_RI) {
        uint32_t run = 0;
        for (uint32_t i = k; i > 0 && t.at(i - 1) == PrUnicodeLineBreakClass_RI; --i)
            ++run;
        if (run % 2 == 1)
            return PrUnicodeLineBreak_Prohibited;
    }
    // LB30b.
    if (b == PrUnicodeLineBreakClass_EM && (a == PrUnicodeLineBreakClass_EB ||
            (t.bases[k - 1].flags & PrUnicodeLineBreakFlag_UnassignedPictographic) != 0))
        return PrUnicodeLineBreak_Prohibited;
    // LB31.
    return PrUnicodeLineBreak_Allowed;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prUnicodeLineBreaks(const uint32_t* codePoints, uint32_t count, PrUnicodeLineBreak* breaks) {
    if (count == 0)
        return;
    // LB1, then LB9 and LB10: each combining sequence is treated as its base, and a mark with no base as alphabetic.
    Array<Base> bases;
    Array<uint8_t> joiners;         // whether each code point is a ZWJ (LB8a)
    for (uint32_t i = 0; i < count; ++i) {
        // The lookup writes the flags, so it runs before they are read.
        PrUnicodeLineBreakFlags flags = 0;
        PrUnicodeLineBreakClass c = prUnicodeLineBreakClass(codePoints[i], &flags);
        c = resolved(c, flags);
        joiners.push(c == PrUnicodeLineBreakClass_ZWJ);
        bool mark = c == PrUnicodeLineBreakClass_CM || c == PrUnicodeLineBreakClass_ZWJ;
        if (mark && bases.count != 0) {
            PrUnicodeLineBreakClass base = bases.last().c;
            if (!isHardBreak(base) && base != PrUnicodeLineBreakClass_SP && base != PrUnicodeLineBreakClass_ZW &&
                    bases.last().last + 1 == i) {
                bases.last().last = i;
                continue;
            }
        }
        if (mark) {
            c = PrUnicodeLineBreakClass_AL;
            flags = 0;
        }
        bases.push(Base{i, i, c, flags, codePoints[i] == 0x25CC});
    }
    Text t{bases.data, bases.count};
    for (uint32_t i = 0; i < count; ++i)
        breaks[i] = PrUnicodeLineBreak_Prohibited;
    for (uint32_t k = 1; k < bases.count; ++k) {
        uint32_t before = bases.data[k].index - 1;
        breaks[before] = between(t, k, joiners.data[before] != 0);
    }
    breaks[count - 1] = PrUnicodeLineBreak_Mandatory;
    bases.reset();
    joiners.reset();
}
