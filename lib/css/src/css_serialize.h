// Serialization
//
// cssom-1 - https://drafts.csswg.org/cssom-1/
// css-color-4 - https://drafts.csswg.org/css-color-4/
// css-syntax-3 - https://drafts.csswg.org/css-syntax/

#pragma once

#include "pr/css.h"
#include "pr/dom.h"
#include "pr/string_view.h"

#include "css/src/css_syntax.h"
#include "css/src/css_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A selector list's compiled records (css_selector.h).
struct CssSelectors;

// ---------------------------------------------------------------------------------------------------------------------
// Values

// Appends the shortest digits that read back as `value` (cssom-1/#serializing-css-values). Exponents from
// kPrF64PositionalMinExponent through kPrF64PositionalMaxExponent use positional notation, and others use `1e+30`
// notation. NaN is written as 0, and an infinity as the largest finite number of its sign.
void cssSerializeNumber(PrTextBuffer* out, double value);

// Appends `text` as an identifier, escaped so that it tokenizes back to `text` (cssom-1/#serialize-an-identifier).
void cssSerializeIdent(PrTextBuffer* out, PrStringView text);

// Returns true if cssSerializeIdent writes `text` unchanged, with no escape.
bool cssIsBareIdent(PrStringView text);

// Appends `text` as a string quoted with '"' (cssom-1/#serialize-a-string).
void cssSerializeString(PrTextBuffer* out, PrStringView text);

// Appends `text` as a url whose argument is serialized as a string (cssom-1/#serialize-a-url).
void cssSerializeUrl(PrTextBuffer* out, PrStringView text);

// Appends a unicode range: "U+" and the bounds in uppercase hexadecimal, joined by '-' unless they are the same code
// point (css-fonts-4/#unicode-range-desc).
void cssSerializeUnicodeRange(PrTextBuffer* out, uint32_t first, uint32_t last);

// Appends an <an+b> in canonical form (css-syntax-3/#serializing-anb), such as `2n+1`, `-n+3` or `4`.
void cssSerializeNth(PrTextBuffer* out, int32_t a, int32_t b);

// ---------------------------------------------------------------------------------------------------------------------
// Value trees (cssom-1/#serializing-css-values)

// Appends a run of values, each serialized as a component value. Values are joined by a space, except that no space
// goes before a ',' (cssom-1/#serialize-a-css-value). Inside a structural production, the joins follow the production:
// no space goes between components written together, the media query rules of cssom-1/#serialize-a-media-query apply,
// and no space goes after an opening bracket or before a closing one.
void cssSerializeValues(PrTextBuffer* out, CssValueRun run);

// Appends the values of a prelude in the same way as cssSerializeValues, with two exceptions. If `urls` is set, a
// <string> at the top level of the run is written as a url, which is how @import and @namespace write it
// (cssom-1/#serialize-a-css-rule). The prelude's complex selectors are written from their compiled records, `selectors`
// from `first` on, in order (cssCompilePreludeSelectors, cssom-1/#serialize-a-selector).
void cssSerializePreludeValues(PrTextBuffer* out, CssValueRun run, bool urls,
    const PrCssSelectors* PR_NULLABLE selectors, uint32_t first);

// ---------------------------------------------------------------------------------------------------------------------
// Tokens

// Appends the token at `index` (css-syntax-3/#serialization). A whitespace token becomes one space.
void cssSerializeToken(PrTextBuffer* out, const CssTokenStream* ts, uint32_t index);

// Appends the tokens that `span` covers, with `/**/` between any two that would otherwise read back as one token
// (css-syntax-3/#serialization).
void cssSerializeTokens(PrTextBuffer* out, const CssTokenStream* ts, CssTokenSpan span);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
