// Values: the words that groups hold, and the compound values a tree interns for the words that name them.

#pragma once

#include "pr/array.h"
#include "pr/bitset.h"
#include "pr/font.h"
#include "pr/hash_index.h"
#include "pr/layout_value.h"
#include "pr/mem.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A compound value: a calculation of lengths and percentages, which reads the base it resolves against.
struct LayoutCompound {
    PrLayoutCalcNode* PR_NULLABLE nodes;   // null while the entry is free
    uint32_t count;
    uint32_t hash;
    uint32_t references;            // number of group entries whose words name it
};

// The compound values of a tree's styles. An index names its entry until the entry is freed.
struct LayoutCompounds {
    Array<LayoutCompound> entries;
    Array<uint32_t> free;           // freed indices, which interning reuses
    HashIndex index;
    Array<uint32_t> candidates;     // same as LayoutTable::candidates
};

// Returns true if a length is auto, none or stretch, or none of the keywords: pixels, a percentage or a compound.
inline bool layoutLengthIsAuto(PrLayoutLength length) {
    return length.bits == prLayoutLengthAuto().bits;
}
inline bool layoutLengthIsNone(PrLayoutLength length) {
    return length.bits == prLayoutLengthNone().bits;
}
inline bool layoutLengthIsStretch(PrLayoutLength length) {
    return length.bits == prLayoutLengthStretch().bits;
}
// Returns true if a length is min-content, max-content or fit-content, which size a box from its content.
inline bool layoutLengthIsContentBased(PrLayoutLength length) {
    return length.bits == prLayoutLengthMinContent().bits || length.bits == prLayoutLengthMaxContent().bits ||
        length.bits == prLayoutLengthFitContent().bits;
}
inline bool layoutLengthIsFixed(PrLayoutLength length) {
    return prLayoutWordKind(length.bits) != PrLayoutWordKind_Keyword;
}

// Returns true if every index below `count` for which `unreferenced` holds is among `candidates`, as
// LayoutTable::candidates requires of a table's list of the entries that a collect frees.
template <typename Unreferenced>
bool layoutCandidatesCover(const Array<uint32_t>& candidates, uint32_t count, Unreferenced unreferenced) {
    Array<uint64_t> listed{};
    listed.resize(uint32_t(bitsetWords(count)));
    memZero(listed.data, listed.count * sizeof(uint64_t));
    for (uint32_t k = 0; k < candidates.count; ++k)
        bitsetSet(listed.data, candidates.data[k]);
    bool covered = true;
    for (uint32_t i = 0; i < count && covered; ++i)
        covered = !unreferenced(i) || bitsetTest(listed.data, i);
    listed.reset();
    return covered;
}

// Interns the calculation of `count` nodes as prLayoutLengthIntern defines.
PrLayoutLength layoutCompoundsIntern(LayoutCompounds* compounds, const PrLayoutCalcNode* nodes, uint32_t count);

// Returns the units that the compound at `index` computes against `base`.
PrLayoutUnit layoutCompoundResolve(const LayoutCompounds* compounds, uint32_t index, PrLayoutUnit base);

// Adds `delta` to the references of the compound that `word` names, if it names one.
void layoutCompoundsCount(LayoutCompounds* compounds, uint32_t word, uint32_t delta);

// Frees the compounds that no group names, of those listed as candidates.
void layoutCompoundsCollect(LayoutCompounds* compounds);

// Returns true if every live compound that no group names is listed as a candidate.
bool layoutCompoundsVerify(const LayoutCompounds* compounds);

// Frees every compound and the table's memory.
void layoutCompoundsReset(LayoutCompounds* compounds);

// Returns the units that `length` resolves to against `base`. Pixels need no base, a percentage scales it, and a
// compound computes with it. A keyword is zero.
inline PrLayoutUnit layoutLengthResolve(const LayoutCompounds* compounds, PrLayoutLength length, PrLayoutUnit base) {
    switch (prLayoutWordKind(length.bits)) {
        case PrLayoutWordKind_Px:
            return prLayoutWordPxValue(length.bits);
        case PrLayoutWordKind_Percent: {
            int64_t scaled = int64_t(base.raw) * (int32_t(length.bits) >> 2) / (int64_t(100) * 65536);
            return PrLayoutUnit{scaled > INT32_MAX ? INT32_MAX : scaled < INT32_MIN ? INT32_MIN : int32_t(scaled)};
        }
        case PrLayoutWordKind_Compound:
            return layoutCompoundResolve(compounds, prLayoutWordCompoundIndex(length.bits), base);
        default:
            return PrLayoutUnit{0};
    }
}

// Returns true if a baseline shift is one of its keywords, rather than a raise.
inline bool layoutBaselineShiftIsKeyword(PrLayoutBaselineShift shift) {
    if (prLayoutWordKind(shift.bits) != PrLayoutWordKind_Keyword)
        return false;
    PrLayoutKeyword keyword = prLayoutWordKeywordValue(shift.bits);
    return keyword >= PrLayoutKeyword_Sub && keyword <= PrLayoutKeyword_Bottom;
}

// Returns the units by which a baseline shift that is not a keyword raises its box. Its percentages are of
// `lineHeight`.
inline PrLayoutUnit layoutBaselineShiftRaise(const LayoutCompounds* compounds, PrLayoutBaselineShift shift,
        PrLayoutUnit lineHeight) {
    return layoutLengthResolve(compounds, PrLayoutLength{shift.bits}, lineHeight);
}

// Returns the height of a line of a font of size `size`, or `normal` if the line height is normal.
inline PrLayoutUnit layoutLineHeight(PrLayoutLineHeight height, PrLayoutUnit size, PrLayoutUnit normal) {
    switch (prLayoutWordKind(height.bits)) {
        case PrLayoutWordKind_Number:
            return prLayoutUnitFromPxDouble(double(prLayoutWordNumberValue(height.bits)) *
                prLayoutUnitToPxDouble(size));
        case PrLayoutWordKind_Px:
            return prLayoutWordPxValue(height.bits);
        default:
            return normal;
    }
}

// Returns true if a line height is normal, which the font gives.
inline bool layoutLineHeightIsNormal(PrLayoutLineHeight height) {
    return height.bits == prLayoutLineHeightNormal().bits;
}

// Returns true if `length` reads the base it resolves against: a percentage, or a compound, which holds one.
inline bool layoutLengthReadsBase(PrLayoutLength length) {
    PrLayoutWordKind kind = prLayoutWordKind(length.bits);
    return kind == PrLayoutWordKind_Percent || kind == PrLayoutWordKind_Compound;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
