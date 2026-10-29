// Values: the one-word encoding of the values that style groups hold, and the calculations a context interns.
//
// A value that is a union of kinds -- pixels, a percentage, a number, a keyword -- is one 32-bit word, so that a group
// is a plain structure that hashes and compares by its bytes. Each typed wrapper holds a word of the kinds it
// documents, and its constructors make them. A calculation that a word cannot hold is interned by a context
// (prLayoutLengthIntern), and the word names it.

#pragma once

#include "pr/base.h"
#include "pr/layout_unit.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Words

// A value that is a union of kinds is one 32-bit word. Its kind is a prefix code in its low bits, and its payload is
// above them, sign-extended where it is signed. Equal values have equal words. Each typed wrapper below holds a word of
// the kinds it documents.
enum {
    PrLayoutWordKind_Px,            // ...0: 31-bit signed 26.6 pixels, as PrLayoutUnit counts them
    PrLayoutWordKind_Percent,       // ...01: 30-bit signed 14.16 percent
    PrLayoutWordKind_Number,        // ...011: a float with its three lowest mantissa bits dropped; no -0 and no NaN
    PrLayoutWordKind_Compound,      // ...0111: a 28-bit index into the context's compound values
    PrLayoutWordKind_Integer,       // ...01111: 27-bit signed
    PrLayoutWordKind_Keyword,       // ...11111: a 27-bit PrLayoutKeyword
};
typedef uint8_t PrLayoutWordKind;

// The keywords a word holds.
enum {
    PrLayoutKeyword_Auto,
    PrLayoutKeyword_None,
    PrLayoutKeyword_Normal,
    PrLayoutKeyword_Stretch,
    PrLayoutKeyword_MinContent,
    PrLayoutKeyword_MaxContent,
    PrLayoutKeyword_FitContent,
    PrLayoutKeyword_Sub,
    PrLayoutKeyword_Super,
    PrLayoutKeyword_Top,
    PrLayoutKeyword_Center,
    PrLayoutKeyword_Bottom,
    PrLayoutKeyword_Content,
};
typedef uint32_t PrLayoutKeyword;

// The largest and smallest payloads of pixels, in units, and of percentages, in units of 1/65536 %.
enum {
    kPrLayoutWordPxMax = 1073741823,
    kPrLayoutWordPxMin = -1073741823 - 1,
    kPrLayoutWordPercentMax = 536870911,
    kPrLayoutWordPercentMin = -536870911 - 1,
};

// Returns the kind of `word`.
PR_CONSTEXPR PrLayoutWordKind prLayoutWordKind(uint32_t word) {
    PrLayoutWordKind kind = 0;
    while (kind < PrLayoutWordKind_Keyword && (word & (1u << kind)) != 0)
        ++kind;
    return kind;
}

// Returns a word of each kind. Every value has a word. Pixels and percentages truncate toward zero to their fixed
// point, which is how a length becomes layout's unit, and saturate at the ends of their kind's range, and a NaN is
// zero. A number drops its three lowest mantissa bits, toward zero, -0 is 0, and a NaN is zero.
PR_CONSTEXPR uint32_t prLayoutWordPxUnits(int32_t units) {
    int32_t raw = units > kPrLayoutWordPxMax ? kPrLayoutWordPxMax
                  : units < kPrLayoutWordPxMin ? kPrLayoutWordPxMin
                                                  : units;
    return PR_VALUE_CAST(uint32_t, raw) << 1;
}
// The conversion of pixels and percentages saturates at run time without testing for the range or NaN. On arm64 under
// Clang, a fixed-point conversion saturates and takes a NaN to zero, and a saturating shift narrows it to the payload,
// through the builtins that arm_neon.h's intrinsics expand to: four instructions. Under MSVC, a saturating conversion
// intrinsic does the same up to the clamp to the payload. Elsewhere, and in a constant expression, the arithmetic is
// spelled out.
#if defined(_MSC_VER) && !defined(__clang__)
int __cdecl _cvt_ftoi_sat(float value);
#endif
PR_CONSTEXPR uint32_t prLayoutWordPx(float px) {
    float units = px * PR_VALUE_CAST(float, kPrLayoutUnitsPerPx);
#if defined(__cplusplus)
    if (!__builtin_is_constant_evaluated())
#endif
    {
#if defined(__aarch64__) && defined(__clang__)
        int32_t fixed = __builtin_bit_cast(int32_t, __builtin_neon_vcvts_n_s32_f32(px, 6));
        return PR_VALUE_CAST(uint32_t, __builtin_bit_cast(int32_t, __builtin_neon_vqshls_n_s32(fixed, 1))) & ~1u;
#elif defined(_MSC_VER) && !defined(__clang__)
        return prLayoutWordPxUnits(_cvt_ftoi_sat(units));
#endif
    }
    int32_t raw = units != units ? 0
                  : units >= PR_VALUE_CAST(float, kPrLayoutWordPxMax) ? kPrLayoutWordPxMax
                  : units <= PR_VALUE_CAST(float, kPrLayoutWordPxMin) ? kPrLayoutWordPxMin
                                                                       : PR_VALUE_CAST(int32_t, units);
    return PR_VALUE_CAST(uint32_t, raw) << 1;
}
PR_CONSTEXPR uint32_t prLayoutWordPercent(float percent) {
    float fixed = percent * 65536;
#if defined(__cplusplus)
    if (!__builtin_is_constant_evaluated())
#endif
    {
#if defined(__aarch64__) && defined(__clang__)
        int32_t converted = __builtin_bit_cast(int32_t, __builtin_neon_vcvts_n_s32_f32(percent, 16));
        int32_t narrowed = __builtin_bit_cast(int32_t, __builtin_neon_vqshls_n_s32(converted, 2));
        return (PR_VALUE_CAST(uint32_t, narrowed) & ~3u) | 1u;
#elif defined(_MSC_VER) && !defined(__clang__)
        int32_t converted = _cvt_ftoi_sat(fixed);
        int32_t raw = converted > kPrLayoutWordPercentMax ? kPrLayoutWordPercentMax
                      : converted < kPrLayoutWordPercentMin ? kPrLayoutWordPercentMin
                                                             : converted;
        return PR_VALUE_CAST(uint32_t, raw) << 2 | 1u;
#endif
    }
    int32_t raw = fixed != fixed ? 0
                  : fixed >= PR_VALUE_CAST(float, kPrLayoutWordPercentMax) ? kPrLayoutWordPercentMax
                  : fixed <= PR_VALUE_CAST(float, kPrLayoutWordPercentMin) ? kPrLayoutWordPercentMin
                                                                            : PR_VALUE_CAST(int32_t, fixed);
    return PR_VALUE_CAST(uint32_t, raw) << 2 | 1u;
}
PR_CONSTEXPR uint32_t prLayoutWordNumber(float number) {
    // Adding +0 makes -0 into +0 and leaves every other number as it is.
    float canonical = number == number ? number + 0.0f : 0.0f;
#if defined(__cplusplus)
    uint32_t bits = __builtin_bit_cast(uint32_t, canonical);
#else
    union {
        float f;
        uint32_t u;
    } pun = {canonical};
    uint32_t bits = pun.u;
#endif
    return (bits & ~7u) | 3u;
}
PR_CONSTEXPR uint32_t prLayoutWordCompound(uint32_t index) {
    return index << 4 | 7u;
}
PR_CONSTEXPR uint32_t prLayoutWordInteger(int32_t value) {
    return PR_VALUE_CAST(uint32_t, value) << 5 | 15u;
}
PR_CONSTEXPR uint32_t prLayoutWordKeyword(PrLayoutKeyword keyword) {
    return keyword << 5 | 31u;
}

// Returns the payload of a word of each kind.
PR_CONSTEXPR PrLayoutUnit prLayoutWordPxValue(uint32_t word) {
    PrLayoutUnit unit = {PR_VALUE_CAST(int32_t, word) >> 1};
    return unit;
}
PR_CONSTEXPR float prLayoutWordPercentValue(uint32_t word) {
    return PR_VALUE_CAST(float, PR_VALUE_CAST(int32_t, word) >> 2) / 65536;
}
PR_CONSTEXPR float prLayoutWordNumberValue(uint32_t word) {
#if defined(__cplusplus)
    return __builtin_bit_cast(float, word & ~7u);
#else
    union {
        uint32_t u;
        float f;
    } pun = {word & ~7u};
    return pun.f;
#endif
}
PR_CONSTEXPR uint32_t prLayoutWordCompoundIndex(uint32_t word) {
    return word >> 4;
}
PR_CONSTEXPR int32_t prLayoutWordIntegerValue(uint32_t word) {
    return PR_VALUE_CAST(int32_t, word) >> 5;
}
PR_CONSTEXPR PrLayoutKeyword prLayoutWordKeywordValue(uint32_t word) {
    return word >> 5;
}

// ---------------------------------------------------------------------------------------------------------------------
// Lengths

PR_NO_PADDING_BEGIN
// A length, as a computed <length-percentage> or sizing value: pixels, a percentage, a compound calculation of both
// (prLayoutLengthIntern), auto, none (the maximum size with no limit), stretch (the size that fills the containing
// block less the margins, css-sizing-3/#stretch-fit-sizing), or min-content, max-content or fit-content (the sizes from
// the box's content, css-sizing-3/#sizing-values).
typedef struct PrLayoutLength {
    uint32_t bits;
} PrLayoutLength;
PR_NO_PADDING_END

// Returns a length of each kind.
PR_CONSTEXPR PrLayoutLength prLayoutLengthPx(float px) {
    PrLayoutLength length = {prLayoutWordPx(px)};
    return length;
}
PR_CONSTEXPR PrLayoutLength prLayoutLengthUnits(PrLayoutUnit unit) {
    PrLayoutLength length = {prLayoutWordPxUnits(unit.raw)};
    return length;
}
PR_CONSTEXPR PrLayoutLength prLayoutLengthPercent(float percent) {
    PrLayoutLength length = {prLayoutWordPercent(percent)};
    return length;
}
PR_CONSTEXPR PrLayoutLength prLayoutLengthAuto(void) {
    PrLayoutLength length = {prLayoutWordKeyword(PrLayoutKeyword_Auto)};
    return length;
}
PR_CONSTEXPR PrLayoutLength prLayoutLengthNone(void) {
    PrLayoutLength length = {prLayoutWordKeyword(PrLayoutKeyword_None)};
    return length;
}
PR_CONSTEXPR PrLayoutLength prLayoutLengthStretch(void) {
    PrLayoutLength length = {prLayoutWordKeyword(PrLayoutKeyword_Stretch)};
    return length;
}
PR_CONSTEXPR PrLayoutLength prLayoutLengthMinContent(void) {
    PrLayoutLength length = {prLayoutWordKeyword(PrLayoutKeyword_MinContent)};
    return length;
}
PR_CONSTEXPR PrLayoutLength prLayoutLengthMaxContent(void) {
    PrLayoutLength length = {prLayoutWordKeyword(PrLayoutKeyword_MaxContent)};
    return length;
}
PR_CONSTEXPR PrLayoutLength prLayoutLengthFitContent(void) {
    PrLayoutLength length = {prLayoutWordKeyword(PrLayoutKeyword_FitContent)};
    return length;
}
PR_CONSTEXPR PrLayoutLength prLayoutLengthNormal(void) {
    PrLayoutLength length = {prLayoutWordKeyword(PrLayoutKeyword_Normal)};
    return length;
}
PR_CONSTEXPR PrLayoutLength prLayoutLengthContent(void) {
    PrLayoutLength length = {prLayoutWordKeyword(PrLayoutKeyword_Content)};
    return length;
}

// ---------------------------------------------------------------------------------------------------------------------
// Baseline shifts

PR_NO_PADDING_BEGIN
// How far an inline-level box moves from where its alignment baseline puts it (css-inline-3/#baseline-shift-property):
// a raise of its baseline -- pixels, a percentage of the box's own line height, or a compound calculation of both
// (prLayoutLengthIntern); sub or super, to where subscripts and superscripts go; or top, center or bottom, which align
// its aligned subtree with the line box instead.
typedef struct PrLayoutBaselineShift {
    uint32_t bits;
} PrLayoutBaselineShift;
PR_NO_PADDING_END

// Returns a baseline shift of each kind.
PR_CONSTEXPR PrLayoutBaselineShift prLayoutBaselineShiftKeyword(PrLayoutKeyword keyword) {
    PrLayoutBaselineShift shift = {prLayoutWordKeyword(keyword)};
    return shift;
}
PR_CONSTEXPR PrLayoutBaselineShift prLayoutBaselineShiftPx(float px) {
    PrLayoutBaselineShift shift = {prLayoutWordPx(px)};
    return shift;
}
PR_CONSTEXPR PrLayoutBaselineShift prLayoutBaselineShiftPercent(float percent) {
    PrLayoutBaselineShift shift = {prLayoutWordPercent(percent)};
    return shift;
}
PR_CONSTEXPR PrLayoutBaselineShift prLayoutBaselineShiftLength(PrLayoutLength length) {
    PrLayoutBaselineShift shift = {length.bits};
    return shift;
}

// ---------------------------------------------------------------------------------------------------------------------
// Numbers

PR_NO_PADDING_BEGIN
// A number.
typedef struct PrLayoutNumber {
    uint32_t bits;
} PrLayoutNumber;
PR_NO_PADDING_END

// Returns a number, or a number's value.
PR_CONSTEXPR PrLayoutNumber prLayoutNumber(float number) {
    PrLayoutNumber value = {prLayoutWordNumber(number)};
    return value;
}
static inline float prLayoutNumberValue(PrLayoutNumber number) {
    return prLayoutWordNumberValue(number.bits);
}

// ---------------------------------------------------------------------------------------------------------------------
// Line heights

PR_NO_PADDING_BEGIN
// The height of a line (css-inline-3/#line-height-property): normal, from the font's metrics; a number, a multiple of
// the font's size; or pixels.
typedef struct PrLayoutLineHeight {
    uint32_t bits;
} PrLayoutLineHeight;
PR_NO_PADDING_END

// Returns a line height of each kind.
PR_CONSTEXPR PrLayoutLineHeight prLayoutLineHeightNormal(void) {
    PrLayoutLineHeight height = {prLayoutWordKeyword(PrLayoutKeyword_Normal)};
    return height;
}
PR_CONSTEXPR PrLayoutLineHeight prLayoutLineHeightNumber(float number) {
    PrLayoutLineHeight height = {prLayoutWordNumber(number)};
    return height;
}
PR_CONSTEXPR PrLayoutLineHeight prLayoutLineHeightPx(float px) {
    PrLayoutLineHeight height = {prLayoutWordPx(px)};
    return height;
}
PR_CONSTEXPR PrLayoutLineHeight prLayoutLineHeightUnits(PrLayoutUnit unit) {
    PrLayoutLineHeight height = {prLayoutWordPxUnits(unit.raw)};
    return height;
}

// ---------------------------------------------------------------------------------------------------------------------
// Track breadths

PR_NO_PADDING_BEGIN
// A breadth of a grid track's sizing function (css-grid-1/#track-sizes): a length of any kind that PrLayoutLength holds
// other than a keyword, auto, min-content, max-content, fit-content (a maximum of max-content that a track's limit
// caps, PrLayoutGridTrack), or a flex factor (`fr`), which a number holds.
typedef struct PrLayoutTrackBreadth {
    uint32_t bits;
} PrLayoutTrackBreadth;
PR_NO_PADDING_END

// Returns a breadth of each kind. A length must be pixels, a percentage or a calculation.
PR_CONSTEXPR PrLayoutTrackBreadth prLayoutTrackBreadthLength(PrLayoutLength length) {
    PrLayoutTrackBreadth breadth = {length.bits};
    return breadth;
}
PR_CONSTEXPR PrLayoutTrackBreadth prLayoutTrackBreadthPx(float px) {
    PrLayoutTrackBreadth breadth = {prLayoutWordPx(px)};
    return breadth;
}
PR_CONSTEXPR PrLayoutTrackBreadth prLayoutTrackBreadthPercent(float percent) {
    PrLayoutTrackBreadth breadth = {prLayoutWordPercent(percent)};
    return breadth;
}
PR_CONSTEXPR PrLayoutTrackBreadth prLayoutTrackBreadthFlex(float fr) {
    PrLayoutTrackBreadth breadth = {prLayoutWordNumber(fr)};
    return breadth;
}
PR_CONSTEXPR PrLayoutTrackBreadth prLayoutTrackBreadthAuto(void) {
    PrLayoutTrackBreadth breadth = {prLayoutWordKeyword(PrLayoutKeyword_Auto)};
    return breadth;
}
PR_CONSTEXPR PrLayoutTrackBreadth prLayoutTrackBreadthMinContent(void) {
    PrLayoutTrackBreadth breadth = {prLayoutWordKeyword(PrLayoutKeyword_MinContent)};
    return breadth;
}
PR_CONSTEXPR PrLayoutTrackBreadth prLayoutTrackBreadthMaxContent(void) {
    PrLayoutTrackBreadth breadth = {prLayoutWordKeyword(PrLayoutKeyword_MaxContent)};
    return breadth;
}
PR_CONSTEXPR PrLayoutTrackBreadth prLayoutTrackBreadthFitContent(void) {
    PrLayoutTrackBreadth breadth = {prLayoutWordKeyword(PrLayoutKeyword_FitContent)};
    return breadth;
}

// ---------------------------------------------------------------------------------------------------------------------
// Calculations

// The operations of a calculation (css-values-4/#calculation-tree). A calculation is its nodes in preorder: each
// operation, then its operands' nodes. It computes in doubles, and only its result becomes units.
enum {
    PrLayoutCalcOp_Value,           // a leaf
    PrLayoutCalcOp_Sum,
    PrLayoutCalcOp_Product,
    PrLayoutCalcOp_Negate,          // one operand
    PrLayoutCalcOp_Invert,          // one operand, a number
    PrLayoutCalcOp_Min,
    PrLayoutCalcOp_Max,
    PrLayoutCalcOp_Clamp,           // three operands: the least, the value, the greatest
    PrLayoutCalcOp_Hypot,
    PrLayoutCalcOp_RoundNearest,    // two operands: the value, and the step it rounds to a multiple of
    PrLayoutCalcOp_RoundUp,
    PrLayoutCalcOp_RoundDown,
    PrLayoutCalcOp_RoundToZero,
    PrLayoutCalcOp_Mod,             // two operands
    PrLayoutCalcOp_Rem,             // two operands
    PrLayoutCalcOp_Abs,             // one operand
    PrLayoutCalcOp_Sign,            // one operand; a number
};
typedef uint8_t PrLayoutCalcOp;

// What a leaf of a calculation is.
enum {
    PrLayoutCalcLeaf_Px,
    PrLayoutCalcLeaf_Percent,
    PrLayoutCalcLeaf_Number,
    PrLayoutCalcLeaf_None,          // a bound of Clamp left out
};
typedef uint8_t PrLayoutCalcLeaf;

PR_NO_PADDING_BEGIN
// A node of a calculation. A node is canonical, as prLayoutCalcLeaf and prLayoutCalcOperation make it, so that equal
// calculations have equal bytes: the fields it does not use are zero.
typedef struct PrLayoutCalcNode {
    PrLayoutCalcOp op;
    PrLayoutCalcLeaf leaf;          // Value: what `value` is
    uint8_t _pad[2];
    uint32_t count;                 // any operation except Value: the number of its operands
    double value;                   // Value: pixels, a percentage, or a number
} PrLayoutCalcNode;
PR_NO_PADDING_END

// Returns a leaf of `kind` holding `value`, or an operation over `count` operands.
PR_CONSTEXPR PrLayoutCalcNode prLayoutCalcLeaf(PrLayoutCalcLeaf kind, double value) {
    PrLayoutCalcNode node = {PrLayoutCalcOp_Value, kind, {0, 0}, 0, kind == PrLayoutCalcLeaf_None ? 0.0 : value};
    return node;
}
PR_CONSTEXPR PrLayoutCalcNode prLayoutCalcOperation(PrLayoutCalcOp op, uint32_t count) {
    PrLayoutCalcNode node = {op, PrLayoutCalcLeaf_Px, {0, 0}, count, 0.0};
    return node;
}

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
