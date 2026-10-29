// Shorthands
//
// css-cascade-5 - https://drafts.csswg.org/css-cascade-5/
// cssom-1 - https://drafts.csswg.org/cssom-1/
//
// A shorthand's value is matched against its grammar, in which each component that sets longhands is marked
// (PrCssValueKind_Longhand). The match is then expanded into a value for each longhand. Serializing reads the
// longhands' values back through the same grammar, or through the prose rules that the shorthand's form names.

#pragma once

#include "pr/text_buffer.h"

#include "css/src/css_property.h"
#include "css/src/css_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The state of a parse (css_parser.h).
struct CssParser;

// A run of properties.
struct CssPropertyList {
    const PrCssProperty* data;
    uint32_t count;
};

// Returns the longhands that `property` sets, in canonical order. Returns the empty list for a longhand.
inline CssPropertyList cssPropertyLonghands(PrCssProperty property) {
    CssPropertySpan span = kCssPropertyLonghands[property];
    return CssPropertyList{kCssPropertyRuns + span.start, span.count};
}

// Returns the shorthands that set `property`, in preferred order. Returns the empty list for a shorthand.
inline CssPropertyList cssPropertyShorthands(PrCssProperty property) {
    CssPropertySpan span = kCssPropertyShorthands[property];
    return CssPropertyList{kCssPropertyRuns + span.start, span.count};
}

// Returns true if `property` is a shorthand.
inline bool cssIsShorthand(PrCssProperty property) {
    return kCssPropertyShorthand[property] != CssShorthand_None;
}

// Returns true if `shorthand` sets `longhand`.
inline bool cssShorthandSets(PrCssProperty shorthand, PrCssProperty longhand) {
    CssPropertyList shorthands = cssPropertyShorthands(longhand);
    for (uint32_t i = 0; i < shorthands.count; ++i) {
        if (shorthands.data[i] == shorthand)
            return true;
    }
    return false;
}

// Returns true if `value` leaves the longhands of `shorthand` holding the shorthand's value until they are computed.
// Such a value contains a substitution function (css-values-5/#pending-substitution-value), or names a system font
// (css-fonts-4/#valdef-font-caption). `value` is not a CSS-wide keyword.
bool cssShorthandPending(PrCssProperty shorthand, CssValueRun value);

// Expands `value` into the value of each longhand of `shorthand` (css-cascade-5/#shorthand-property). `value` is a
// match of the shorthand's grammar in the parser's pool, and is neither a CSS-wide keyword nor pending. Afterwards the
// parser's `expanded` holds the values in canonical order, as spans of its pool. Returns false if the shorthand's prose
// rules reject the value.
bool cssExpandShorthand(CssParser* parser, PrCssProperty shorthand, CssValueSpan value);

// Appends the value of `shorthand`, serialized from the values of its longhands (cssom-1/#serialize-a-css-value).
// `values` holds one value for each longhand, in canonical order. Appends nothing if the shorthand cannot represent
// those values.
void cssSerializeShorthand(TextBuffer* out, PrCssProperty shorthand, const CssValueRun* values);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
