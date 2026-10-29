// Presentational hints: the attributes of HTML elements that map to CSS properties
//
// html - https://html.spec.whatwg.org/multipage/rendering.html

#pragma once

#include "pr/color.h"
#include "pr/string_view.h"

#include "css/src/css_hints.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the provider of the hints of HTML elements, which HTML gives each document's CSS.
const CssHintProvider* htmlHints();

// Parses `text` by the rules for parsing a legacy color value (html/#rules-for-parsing-a-legacy-colour-value). Returns
// false if it gives no color.
bool htmlParseLegacyColor(PrStringView text, PrColor8* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
