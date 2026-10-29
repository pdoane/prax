// CSS Units
//
// css-conditional-5 - https://drafts.csswg.org/css-conditional-5/
// css-grid-1 - https://drafts.csswg.org/css-grid-1/
// css-speech-1 - https://drafts.csswg.org/css-speech-1/
// css-values-4 - https://drafts.csswg.org/css-values-4/

#pragma once

#include "pr/base.h"
#include "pr/css.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The quantity that a unit measures. It decides the type that a dimension parses as.
enum {
    CssUnitCategory_Unknown,
    CssUnitCategory_Number,
    CssUnitCategory_Percent,
    CssUnitCategory_Length,
    CssUnitCategory_Angle,
    CssUnitCategory_Time,
    CssUnitCategory_Frequency,
    CssUnitCategory_Resolution,
    CssUnitCategory_Flex,
    CssUnitCategory_Decibel,
    CssUnitCategory_Semitones,
};
typedef uint8_t CssUnitCategory;

// The length of the longest unit.
constexpr uint32_t kCssUnitMaxLength = 5;

// Returns the unit that `text` names, or PrCssUnit_Unknown if none matches. Units are ASCII case-insensitive.
PrCssUnit cssUnitFromText(PrStringView text);

// Returns the unit's canonical spelling. `unit` must be one that the tables name, from PrCssUnit_Em on.
PrStringView cssUnitName(PrCssUnit unit);

// Returns the category of `unit`: CssUnitCategory_Number for PrCssUnit_Number, and CssUnitCategory_Unknown for
// PrCssUnit_Unknown.
CssUnitCategory cssUnitCategory(PrCssUnit unit);

// Returns the size of one `unit` in the canonical unit of its category (css-values-4/#canonical-unit). Returns 0 for a
// unit with no fixed ratio to the canonical unit: a relative length, PrCssUnit_Number and PrCssUnit_Unknown.
double cssUnitScale(PrCssUnit unit);

// Returns the canonical unit of a category (css-values-4/#canonical-unit), or PrCssUnit_Number for a category that has
// none: numbers, percentages and unknown units.
PrCssUnit cssUnitCanonical(CssUnitCategory category);

// Returns true if `unit` is relative to the viewport (css-values-4/#viewport-relative-lengths).
bool cssUnitViewportRelative(PrCssUnit unit);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
