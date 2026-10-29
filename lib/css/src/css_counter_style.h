// Counter styles: how a counter's value is written as text.
//
// css-counter-styles-3 - https://drafts.csswg.org/css-counter-styles-3/

#pragma once

#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The algorithm that turns a value into a representation (css-counter-styles-3/#counter-style-system).
enum {
    CssCounterSystem_Cyclic,
    CssCounterSystem_Fixed,
    CssCounterSystem_Symbolic,
    CssCounterSystem_Alphabetic,
    CssCounterSystem_Numeric,
    CssCounterSystem_Additive,

    // The algorithms of the complex predefined styles (css-counter-styles-3/#complex-predefined-counters), which no
    // rule names, and which a rule that extends such a style keeps. Each longhand CJK style takes the algorithm of its
    // language over the extended range (#extended-range-optional), with the digits 0 to 9, the second, third and fourth
    // digit markers, and the second, third and fourth group markers as its symbols. An informal style drops a digit 1
    // that its language drops. ethiopic-numeric's (#ethiopic-numeric-counter-style) takes the units 1 to 9, the tens 10
    // to 90, and the separators after an odd and an even group of two digits.
    CssCounterSystem_ChineseInformal,
    CssCounterSystem_ChineseFormal,
    CssCounterSystem_JapaneseInformal,
    CssCounterSystem_JapaneseFormal,
    CssCounterSystem_KoreanInformal,
    CssCounterSystem_KoreanFormal,

    // hebrew's past 10999, which css-counter-styles-3/#simple-numeric allows: the thousands as a value below 1000 by
    // its additive tuples, a geresh, and the rest.
    CssCounterSystem_Hebrew,
    CssCounterSystem_Ethiopic,
};
typedef uint8_t CssCounterSystem;

// Where the one symbol of a style points, which a representation turns to the writing mode and direction it is in:
// nowhere, so that it does not turn, to the inline end, or to the block end. The disclosure styles' symbols point
// (css-counter-styles-3/#simple-symbolic).
enum {
    CssCounterPointing_None,
    CssCounterPointing_InlineEnd,
    CssCounterPointing_BlockEnd,
};
typedef uint8_t CssCounterPointing;

// A weight and the symbol that represents it in an additive style (css-counter-styles-3/#additive-tuple).
struct CssAdditiveTuple {
    int32_t weight;
    PrStringView symbol;
};

// A range of values that a style represents, inclusive. INT32_MIN and INT32_MAX stand for infinite bounds.
struct CssCounterRange {
    int32_t low;
    int32_t high;
};

// A counter style whose `extends` is resolved: the descriptors of its rule, with the initial value of each descriptor
// that the rule leaves out. A null view is an empty symbol.
struct CssCounterStyle {
    PrStringView name;
    CssCounterSystem system;
    int32_t first;                              // the value of the first symbol of a fixed style

    const PrStringView* PR_NULLABLE symbols;
    uint32_t symbolCount;
    CssCounterPointing pointing;                // where a symbol that points turns, or None

    const CssAdditiveTuple* PR_NULLABLE additive; // in order of decreasing weight
    uint32_t additiveCount;

    PrStringView negative[2];                   // the symbols before and after a negative value's representation
    PrStringView prefix;
    PrStringView suffix;

    // The ranges of values that the style represents. With none, the range is the auto range of the system
    // (css-counter-styles-3/#valdef-counter-style-range-auto).
    const CssCounterRange* PR_NULLABLE ranges;
    uint32_t rangeCount;

    // The number of grapheme clusters that padding brings a shorter representation up to, and the symbol that pads it
    // (css-counter-styles-3/#counter-style-pad).
    uint32_t minimumLength;
    PrStringView padSymbol;

    PrStringView fallback;                      // the name of the style that represents a value outside the range
};

// Returns the predefined counter style that `name` names (css-counter-styles-3/#predefined-counters), matched ASCII
// case-insensitively, or null if no predefined style has the name.
const CssCounterStyle* PR_NULLABLE cssCounterStylePredefined(PrStringView name);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
